/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include "flashlight/fl/flashlight.h"

#include "flashlight/app/asr/common/Defines.h"
#include "flashlight/app/asr/common/Flags.h"
#include "flashlight/app/asr/criterion/criterion.h"
#include "flashlight/app/asr/data/FeatureTransforms.h"
#include "flashlight/app/asr/data/Utils.h"
#include "flashlight/app/asr/decoder/Defines.h"
#include "flashlight/app/asr/decoder/TranscriptionUtils.h"
#include "flashlight/app/asr/runtime/runtime.h"
#include "flashlight/ext/common/DistributedUtils.h"
#include "flashlight/ext/common/SequentialBuilder.h"
#include "flashlight/ext/common/Serializer.h"
#include "flashlight/ext/plugin/ModulePlugin.h"
#include "flashlight/lib/common/System.h"
#include "flashlight/lib/text/dictionary/Dictionary.h"
#include "flashlight/lib/text/dictionary/Utils.h"

using fl::ext::afToVector;
using fl::ext::Serializer;
using fl::lib::join;
using fl::lib::pathsConcat;

using namespace fl::app::asr;

int main(int argc, char** argv) {
  fl::init();
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  std::string exec(argv[0]);
  std::vector<std::string> argvs;
  for (int i = 0; i < argc; i++) {
    argvs.emplace_back(argv[i]);
  }
  gflags::SetUsageMessage("Usage: Please refer to https://git.io/JvJuR");
  if (argc <= 1) {
    LOG(FATAL) << gflags::ProgramUsage();
  }

  /* ===================== Parse Options ===================== */
  LOG(INFO) << "Parsing command line flags";
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  auto flagsfile = FLAGS_flagsfile;
  if (!flagsfile.empty()) {
    LOG(INFO) << "Reading flags from file " << flagsfile;
    gflags::ReadFromFlagsFile(flagsfile, argv[0], true);
  }

  if (!FLAGS_fl_log_level.empty()) {
    fl::Logging::setMaxLoggingLevel(fl::logLevelValue(FLAGS_fl_log_level));
  }
  fl::VerboseLogging::setMaxLoggingLevel(FLAGS_fl_vlog_level);

  /* ===================== Create Network ===================== */
  std::shared_ptr<fl::Module> network;
  std::shared_ptr<SequenceCriterion> criterion;
  std::unordered_map<std::string, std::string> cfg;
  std::string version;
  bool usePlugin = false;
  LOG(INFO) << "[Network] Reading acoustic model from " << FLAGS_am;
  fl::setDevice(0);
  if (fl::lib::endsWith(FLAGS_arch, ".so")) {
    usePlugin = true;
    (void)fl::ext::ModulePlugin(FLAGS_arch);
  }
  Serializer::load(FLAGS_am, version, cfg, network, criterion);
  if (version != FL_APP_ASR_VERSION) {
    LOG(WARNING) << "[Network] Model version " << version
                 << " and code version " << FL_APP_ASR_VERSION;
  }
  network->eval();
  criterion->eval();

  LOG(INFO) << "[Network] " << network->prettyString();
  LOG(INFO) << "[Criterion] " << criterion->prettyString();
  LOG(INFO) << "[Network] Number of params: " << numTotalParams(network);

  auto flags = cfg.find(kGflags);
  if (flags == cfg.end()) {
    LOG(FATAL) << "[Network] Invalid config loaded from " << FLAGS_am;
  }
  LOG(INFO) << "[Network] Updating flags from config file: " << FLAGS_am;
  gflags::ReadFlagsFromString(flags->second, gflags::GetArgv0(), true);

  // override with user-specified flags
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  if (!flagsfile.empty()) {
    gflags::ReadFromFlagsFile(flagsfile, argv[0], true);
  }

  // Only Copy any values from deprecated flags to new flags when deprecated
  // flags are present and corresponding new flags aren't
  handleDeprecatedFlags();

  LOG(INFO) << "Gflags after parsing \n" << serializeGflags("; ");

  /* ===================== Create Dictionary ===================== */
  auto dictPath = FLAGS_tokens;
  if (dictPath.empty() || !fl::lib::fileExists(dictPath)) {
    throw std::runtime_error("Invalid dictionary filepath specified.");
  }
  fl::lib::text::Dictionary tokenDict(dictPath);
  // Setup-specific modifications
  for (int64_t r = 1; r <= FLAGS_replabel; ++r) {
    tokenDict.addEntry("<" + std::to_string(r) + ">");
  }
  // ctc expects the blank label last
  if (FLAGS_criterion == kCtcCriterion) {
    tokenDict.addEntry(kBlankToken);
  }

  bool isSeq2seqCrit = FLAGS_criterion == kSeq2SeqTransformerCriterion ||
      FLAGS_criterion == kSeq2SeqRNNCriterion;
  if (isSeq2seqCrit) {
    tokenDict.addEntry(fl::app::asr::kEosToken);
    tokenDict.addEntry(fl::lib::text::kPadToken);
  }

  int numClasses = tokenDict.indexSize();
  LOG(INFO) << "Number of classes (network): " << numClasses;

  fl::lib::text::Dictionary wordDict;
  fl::lib::text::LexiconMap lexicon;
  if (!FLAGS_lexicon.empty()) {
    lexicon = fl::lib::text::loadWords(FLAGS_lexicon, FLAGS_maxword);
    wordDict = fl::lib::text::createWordDict(lexicon);
    LOG(INFO) << "Number of words: " << wordDict.indexSize();
  }

  fl::lib::text::DictionaryMap dicts = {{kTargetIdx, tokenDict},
                                        {kWordIdx, wordDict}};

  /* ===================== Create Dataset ===================== */
  fl::lib::audio::FeatureParams featParams(
      FLAGS_samplerate,
      FLAGS_framesizems,
      FLAGS_framestridems,
      FLAGS_filterbanks,
      FLAGS_lowfreqfilterbank,
      FLAGS_highfreqfilterbank,
      FLAGS_mfcccoeffs,
      kLifterParam /* lifterparam */,
      FLAGS_devwin /* delta window */,
      FLAGS_devwin /* delta-delta window */);
  featParams.useEnergy = false;
  featParams.usePower = false;
  featParams.zeroMeanFrame = false;
  FeatureType featType =
      getFeatureType(FLAGS_features_type, FLAGS_channels, featParams).second;

  TargetGenerationConfig targetGenConfig(
      FLAGS_wordseparator,
      FLAGS_sampletarget,
      FLAGS_criterion,
      FLAGS_surround,
      isSeq2seqCrit,
      FLAGS_replabel,
      true /* skip unk */,
      FLAGS_usewordpiece /* fallback2LetterWordSepLeft */,
      !FLAGS_usewordpiece /* fallback2LetterWordSepLeft */);

  auto inputTransform = inputFeatures(
      featParams,
      featType,
      {FLAGS_localnrmlleftctx, FLAGS_localnrmlrightctx},
      /*sfxConf=*/{});
  auto targetTransform = targetFeatures(tokenDict, lexicon, targetGenConfig);
  auto wordTransform = wordFeatures(wordDict);
  int targetpadVal =
      isSeq2seqCrit ? tokenDict.getIndex(fl::lib::text::kPadToken) : kTargetPadValue;
  int wordpadVal = wordDict.getIndex(fl::lib::text::kUnkToken);

  std::vector<std::string> testSplits = fl::lib::split(",", FLAGS_test, true);
  auto ds = createDataset(
      testSplits,
      FLAGS_datadir,
      1 /* batchsize */,
      inputTransform,
      targetTransform,
      wordTransform,
      std::make_tuple(0, targetpadVal, wordpadVal),
      0 /* worldrank */,
      1 /* worldsize */);

  int nSamples = ds->size();
  if (FLAGS_maxload > 0) {
    nSamples = std::min(nSamples, FLAGS_maxload);
  }
  LOG(INFO) << "[Dataset] Dataset loaded, with " << nSamples << " samples.";

  /* ===================== Test ===================== */
  std::vector<double> sliceWrdDst(FLAGS_nthread_decoder_am_forward);
  std::vector<double> sliceTknDst(FLAGS_nthread_decoder_am_forward);
  std::vector<int> sliceNumWords(FLAGS_nthread_decoder_am_forward, 0);
  std::vector<int> sliceNumTokens(FLAGS_nthread_decoder_am_forward, 0);
  std::vector<int> sliceNumSamples(FLAGS_nthread_decoder_am_forward, 0);
  std::vector<double> sliceTime(FLAGS_nthread_decoder_am_forward, 0);

  auto cleanTestPath = cleanFilepath(FLAGS_test);
  std::string emissionDir;
  if (!FLAGS_emission_dir.empty()) {
    emissionDir = pathsConcat(FLAGS_emission_dir, cleanTestPath);
    fl::lib::dirCreate(emissionDir);
  }

  // Prepare sclite log writer
  std::ofstream hypStream, refStream;
  if (!FLAGS_sclite.empty()) {
    auto hypPath = pathsConcat(FLAGS_sclite, cleanTestPath + ".hyp");
    auto refPath = pathsConcat(FLAGS_sclite, cleanTestPath + ".viterbi.ref");
    hypStream.open(hypPath);
    refStream.open(refPath);
    if (!hypStream.is_open() || !hypStream.good()) {
      LOG(FATAL) << "Error opening hypothesis file: " << hypPath;
    }
    if (!refStream.is_open() || !refStream.good()) {
      LOG(FATAL) << "Error opening reference file: " << refPath;
    }
  }

  std::mutex hypMutex, refMutex;
  auto writeHyp = [&hypMutex, &hypStream](const std::string& hypStr) {
    std::lock_guard<std::mutex> lock(hypMutex);
    hypStream << hypStr;
  };
  auto writeRef = [&refMutex, &refStream](const std::string& refStr) {
    std::lock_guard<std::mutex> lock(refMutex);
    refStream << refStr;
  };

  // Run test
  auto run = [&network,
              &usePlugin,
              &criterion,
              &nSamples,
              &ds,
              &tokenDict,
              &wordDict,
              &writeHyp,
              &writeRef,
              &emissionDir,
              &sliceWrdDst,
              &sliceTknDst,
              &sliceNumWords,
              &sliceNumTokens,
              &sliceNumSamples,
              &sliceTime,
              &isSeq2seqCrit](int tid) {
    // Initialize AM
    fl::setDevice(tid);
    std::shared_ptr<fl::Module> localNetwork = network;
    std::shared_ptr<SequenceCriterion> localCriterion = criterion;
    if (tid != 0) {
      std::unordered_map<std::string, std::string> dummyCfg;
      std::string dummyVersion;
      Serializer::load(
          FLAGS_am, dummyVersion, dummyCfg, localNetwork, localCriterion);
      localNetwork->eval();
      localCriterion->eval();
    }

    std::vector<int64_t> selectedIds;
    for (int64_t i = tid; i < nSamples; i += FLAGS_nthread_decoder_am_forward) {
      selectedIds.emplace_back(i);
    }
    std::shared_ptr<fl::Dataset> localDs =
        std::make_shared<fl::ResampleDataset>(ds, selectedIds);
    localDs = std::make_shared<fl::PrefetchDataset>(
        localDs, FLAGS_nthread, FLAGS_nthread);

    TestMeters meters;
    meters.timer.resume();
    int cnt = 0;
    for (auto& sample : *localDs) {
      fl::Variable rawEmission;
      if (usePlugin) {
        rawEmission = localNetwork
                          ->forward({fl::input(sample[kInputIdx]),
                                     fl::noGrad(sample[kDurationIdx])})
                          .front();
      } else {
        rawEmission = fl::ext::forwardSequentialModuleWithPadMask(
            fl::input(sample[kInputIdx]), localNetwork, sample[kDurationIdx]);
      }
      auto emission = afToVector<float>(rawEmission);
      auto tokenTarget = afToVector<int>(sample[kTargetIdx]);
      auto wordTarget = afToVector<int>(sample[kWordIdx]);
      auto sampleId = readSampleIds(sample[kSampleIdx]).front();

      auto letterTarget = tknTarget2Ltr(
          tokenTarget,
          tokenDict,
          FLAGS_criterion,
          FLAGS_surround,
          isSeq2seqCrit,
          FLAGS_replabel,
          FLAGS_usewordpiece,
          FLAGS_wordseparator);
      std::vector<std::string> wordTargetStr;
      if (FLAGS_uselexicon) {
        wordTargetStr = wrdIdx2Wrd(wordTarget, wordDict);
      } else {
        wordTargetStr = tkn2Wrd(letterTarget, FLAGS_wordseparator);
      }

      // Tokens
      auto tokenPrediction =
          afToVector<int>(localCriterion->viterbiPath(rawEmission.array()));
      auto letterPrediction = tknPrediction2Ltr(
          tokenPrediction,
          tokenDict,
          FLAGS_criterion,
          FLAGS_surround,
          isSeq2seqCrit,
          FLAGS_replabel,
          FLAGS_usewordpiece,
          FLAGS_wordseparator);

      meters.tknDstSlice.add(letterPrediction, letterTarget);

      // Words
      std::vector<std::string> wrdPredictionStr =
          tkn2Wrd(letterPrediction, FLAGS_wordseparator);
      meters.wrdDstSlice.add(wrdPredictionStr, wordTargetStr);

      if (!FLAGS_sclite.empty()) {
        writeRef(join(" ", wordTargetStr) + " (" + sampleId + ")\n");
        writeHyp(join(" ", wrdPredictionStr) + " (" + sampleId + ")\n");
      }

      if (FLAGS_show) {
        meters.tknDst.reset();
        meters.wrdDst.reset();
        meters.tknDst.add(letterPrediction, letterTarget);
        meters.wrdDst.add(wrdPredictionStr, wordTargetStr);

        std::cout << "|T|: " << join(" ", letterTarget) << std::endl;
        std::cout << "|P|: " << join(" ", letterPrediction) << std::endl;
        std::cout << "[sample: " << sampleId
                  << ", WER: " << meters.wrdDst.errorRate()[0]
                  << "\%, TER: " << meters.tknDst.errorRate()[0]
                  << "\%, total WER: " << meters.wrdDstSlice.errorRate()[0]
                  << "\%, total TER: " << meters.tknDstSlice.errorRate()[0]
                  << "\%, progress (thread " << tid << "): "
                  << static_cast<float>(++cnt) / selectedIds.size() * 100
                  << "\%]" << std::endl;
      }

      /* Save emission and targets */
      int nTokens = rawEmission.dims(0);
      int nFrames = rawEmission.dims(1);
      EmissionUnit emissionUnit(emission, sampleId, nFrames, nTokens);

      // Update counters
      sliceNumWords[tid] += wordTarget.size();
      sliceNumTokens[tid] += letterTarget.size();
      sliceNumSamples[tid]++;

      if (!emissionDir.empty()) {
        std::string savePath = pathsConcat(emissionDir, sampleId + ".bin");
        Serializer::save(savePath, FL_APP_ASR_VERSION, emissionUnit);
      }
    }

    meters.timer.stop();

    sliceWrdDst[tid] = meters.wrdDstSlice.value()[0];
    sliceTknDst[tid] = meters.tknDstSlice.value()[0];
    sliceTime[tid] = meters.timer.value();
  };

  /* Spread threades */
  // TODO possibly try catch for futures to proper logging of all errors
  // https://github.com/facebookresearch/gtn/blob/master/gtn/parallel/parallel_map.h#L154
  auto startThreadsAndJoin = [&run](int nThreads) {
    if (nThreads == 1) {
      run(0);
    } else if (nThreads > 1) {
      std::vector<std::future<void>> futs(nThreads);
      fl::ThreadPool threadPool(nThreads);
      for (int i = 0; i < nThreads; i++) {
        futs[i] = threadPool.enqueue(run, i);
      }
      for (int i = 0; i < nThreads; i++) {
        futs[i].get();
      }
    } else {
      LOG(FATAL) << "Invalid negative FLAGS_nthread_decoder_am_forward";
    }
  };
  auto timer = fl::TimeMeter();
  timer.resume();
  startThreadsAndJoin(FLAGS_nthread_decoder_am_forward);
  timer.stop();

  int totalTokens = 0, totalWords = 0, totalSamples = 0;
  for (int i = 0; i < FLAGS_nthread_decoder_am_forward; i++) {
    totalTokens += sliceNumTokens[i];
    totalWords += sliceNumWords[i];
    totalSamples += sliceNumSamples[i];
  }
  double totalWer = 0, totalTer = 0, totalTime = 0;
  for (int i = 0; i < FLAGS_nthread_decoder_am_forward; i++) {
    totalWer += sliceWrdDst[i];
    totalTer += sliceTknDst[i];
    totalTime += sliceTime[i];
  }
  if (totalWer > 0 && totalWords == 0) {
    totalWer = std::numeric_limits<double>::infinity();
  } else {
    totalWer = totalWords > 0 ? totalWer / totalWords * 100. : 0.0;
  }
  if (totalTer > 0 && totalTokens == 0) {
    totalTer = std::numeric_limits<double>::infinity();
  } else {
    totalTer = totalTokens > 0 ? totalTer / totalTokens * 100. : 0.0;
  }
  LOG(INFO) << "------";
  LOG(INFO) << "[Test " << FLAGS_test << " (" << totalSamples << " samples) in "
            << timer.value() << "s (actual decoding time "
            << std::setprecision(3) << totalTime / totalSamples
            << "s/sample) -- WER: " << std::setprecision(6) << totalWer
            << "\%, TER: " << totalTer << "\%]";

  return 0;
}
