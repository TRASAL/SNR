// Copyright 2017 Netherlands Institute for Radio Astronomy (ASTRON)
// Copyright 2017 Netherlands eScience Center
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <string>
#include <vector>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <algorithm>

#include <configuration.hpp>

#include <ArgumentList.hpp>
#include <Observation.hpp>
#include <InitializeOpenCL.hpp>
#include <Kernel.hpp>
#include <SNR.hpp>
#include <utils.hpp>
#include <Timer.hpp>

void initializeDeviceMemoryD(cl::Context &clContext, cl::CommandQueue *clQueue, std::vector<inputDataType> *input, cl::Buffer *input_d, cl::Buffer *outputValue_d, const uint64_t output_size);
void initializeDeviceMemoryD(cl::Context &clContext, cl::CommandQueue *clQueue, std::vector<inputDataType> *input, cl::Buffer *input_d, cl::Buffer *outputValue_d, const uint64_t output_size, cl::Buffer *outputSample_d, const uint64_t outputSample_size);
void initializeDeviceMemoryD(cl::Context &clContext, cl::CommandQueue *clQueue, std::vector<inputDataType> *input, cl::Buffer *input_d, cl::Buffer *outputValue_d, cl::Buffer *outputStd_d, const uint64_t output_size, cl::Buffer *outputSample_d, const uint64_t outputSample_size);
void initializeDeviceMemoryD(cl::Context &clContext, cl::CommandQueue *clQueue, std::vector<inputDataType> *input, cl::Buffer *input_d, cl::Buffer *outputValue_d, const uint64_t outputSNR_size, cl::Buffer *baselines_d, std::vector<outputDataType> *baselines);
int tune(const bool bestMode, const unsigned int nrIterations, const unsigned int minThreads, const unsigned int maxThreads, const unsigned int maxItems, const unsigned int clPlatformID, const unsigned int clDeviceID, const SNR::DataOrdering ordering, const SNR::Kernel kernelTuned, const unsigned int padding, const AstroData::Observation &observation, SNR::snrConf &conf, const unsigned int medianStep = 0, const float nSigma = 3.0f);

int main(int argc, char *argv[])
{
    int returnCode = 0;
    bool bestMode = false;
    unsigned int padding = 0;
    unsigned int nrIterations = 0;
    unsigned int clPlatformID = 0;
    unsigned int clDeviceID = 0;
    unsigned int minThreads = 0;
    unsigned int maxItems = 0;
    unsigned int maxThreads = 0;
    unsigned int stepSize = 0;
    float nSigma = 3.0f;
    SNR::Kernel kernel;
    SNR::DataOrdering ordering;
    SNR::snrConf conf;
    AstroData::Observation observation;

    try
    {
        isa::utils::ArgumentList args(argc, argv);
        if (args.getSwitch("-snr"))
        {
            kernel = SNR::Kernel::SNR;
        }
        else if ( args.getSwitch("-snr_sc") )
        {
            kernel = SNR::Kernel::SNRSigmaCut;
        }
        else if (args.getSwitch("-max"))
        {
            kernel = SNR::Kernel::Max;
        }
        else if (args.getSwitch("-max_std"))
        {
            kernel = SNR::Kernel::MaxStdSigmaCut;
        }
        else if (args.getSwitch("-median"))
        {
            kernel = SNR::Kernel::MedianOfMedians;
        }
        else if (args.getSwitch("-momad"))
        {
            kernel = SNR::Kernel::MedianOfMediansAbsoluteDeviation;
        }
        else if (args.getSwitch("-absolute_deviation"))
        {
            kernel = SNR::Kernel::AbsoluteDeviation;
        }
        else
        {
            std::cerr << "One switch between -snr -snr_sc -max -max_std -median -momad and -absolute_deviation is required." << std::endl;
            return 1;
        }
        if (args.getSwitch("-dms_samples"))
        {
            ordering = SNR::DataOrdering::DMsSamples;
        }
        else if (args.getSwitch("-samples_dms"))
        {
            ordering = SNR::DataOrdering::SamplesDMs;
        }
        else
        {
            std::cerr << "One switch between -dms_samples and -samples_dms is required." << std::endl;
            return 1;
        }
        nrIterations = args.getSwitchArgument<unsigned int>("-iterations");
        clPlatformID = args.getSwitchArgument<unsigned int>("-opencl_platform");
        clDeviceID = args.getSwitchArgument<unsigned int>("-opencl_device");
        bestMode = args.getSwitch("-best");
        padding = args.getSwitchArgument<unsigned int>("-padding");
        minThreads = args.getSwitchArgument<unsigned int>("-min_threads");
        if (kernel == SNR::Kernel::SNR || kernel == SNR::Kernel::SNRSigmaCut || kernel == SNR::Kernel::Max || kernel == SNR::Kernel::MaxStdSigmaCut || kernel == SNR::Kernel::AbsoluteDeviation)
        {
            maxItems = args.getSwitchArgument<unsigned int>("-max_items");
        }
        else
        {
            maxItems = 1;
        }
        maxThreads = args.getSwitchArgument<unsigned int>("-max_threads");
        conf.setSubbandDedispersion(args.getSwitch("-subband"));
        observation.setNrSynthesizedBeams(args.getSwitchArgument<unsigned int>("-beams"));
        observation.setNrSamplesPerBatch(args.getSwitchArgument<unsigned int>("-samples"));
        if (conf.getSubbandDedispersion())
        {
            observation.setDMRange(args.getSwitchArgument<unsigned int>("-subbanding_dms"), 0.0f, 0.0f, true);
        }
        else
        {
            observation.setDMRange(1, 0.0f, 0.0f, true);
        }
        observation.setDMRange(args.getSwitchArgument<unsigned int>("-dms"), 0.0, 0.0);
        if (kernel == SNR::Kernel::MedianOfMedians || kernel == SNR::Kernel::MedianOfMediansAbsoluteDeviation)
        {
            stepSize = args.getSwitchArgument<unsigned int>("-median_step");
        }
        else if ( kernel == SNR::Kernel::SNRSigmaCut || kernel == SNR::Kernel::MaxStdSigmaCut )
        {
          nSigma = args.getSwitchArgument<float>("-nsigma");
        }
    }
    catch (isa::utils::EmptyCommandLine &err)
    {
        std::cerr << "Usage: " << argv[0] << " [-snr | -snr_sc | -max | -max_std | -median | -momad | -absolute_deviation] [-dms_samples | -samples_dms] [-best] -iterations <int> -opencl_platform <int> -opencl_device <int> -padding <int> -min_threads <int> -max_threads <int> -max_items <int> [-subband] -beams <int> -dms <int> -samples <int>" << std::endl;
        std::cerr << "\t -subband -subbanding_dms <int>" << std::endl;
        std::cerr << "\t -snr_sc -nsigma <float>" << std::endl;
        std::cerr << "\t -median -median_step <int>" << std::endl;
        std::cerr << "\t -momad -median_step <int" << std::endl;
        std::cerr << "\t -max_std -nsigma <float>" << std::endl;
        return 1;
    }
    catch (std::exception &err)
    {
        std::cerr << err.what() << std::endl;
        return 1;
    }
    if (kernel == SNR::Kernel::SNR || kernel == SNR::Kernel::Max || kernel == SNR::Kernel::AbsoluteDeviation)
    {
        returnCode = tune(bestMode, nrIterations, minThreads, maxThreads, maxItems, clPlatformID, clDeviceID, ordering, kernel, padding, observation, conf);
    }
    else if (kernel == SNR::Kernel::MedianOfMedians || kernel == SNR::Kernel::MedianOfMediansAbsoluteDeviation)
    {
        returnCode = tune(bestMode, nrIterations, minThreads, maxThreads, maxItems, clPlatformID, clDeviceID, ordering, kernel, padding, observation, conf, stepSize);
    }
    else if ( kernel == SNR::Kernel::SNRSigmaCut || kernel == SNR::Kernel::MaxStdSigmaCut )
    {
        returnCode = tune(bestMode, nrIterations, minThreads, maxThreads, maxItems, clPlatformID, clDeviceID, ordering, kernel, padding, observation, conf, 0, nSigma);
    }

    return returnCode;
}

void initializeDeviceMemoryD(cl::Context &clContext, cl::CommandQueue *clQueue, std::vector<inputDataType> *input, cl::Buffer *input_d, cl::Buffer *outputValue_d, const uint64_t output_size)
{
    try
    {
        *input_d = cl::Buffer(clContext, CL_MEM_READ_WRITE, input->size() * sizeof(inputDataType), 0, 0);
        *outputValue_d = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, output_size * sizeof(outputDataType), 0, 0);
        clQueue->enqueueWriteBuffer(*input_d, CL_FALSE, 0, input->size() * sizeof(inputDataType), reinterpret_cast<void *>(input->data()));
        clQueue->finish();
    }
    catch (cl::Error &err)
    {
        std::cerr << "OpenCL error: " << std::to_string(err.err()) << "." << std::endl;
        throw;
    }
}

void initializeDeviceMemoryD(cl::Context &clContext, cl::CommandQueue *clQueue, std::vector<inputDataType> *input, cl::Buffer *input_d, cl::Buffer *outputValue_d, const uint64_t output_size, cl::Buffer *outputSample_d, const uint64_t outputSample_size)
{
    try
    {
        *input_d = cl::Buffer(clContext, CL_MEM_READ_WRITE, input->size() * sizeof(inputDataType), 0, 0);
        *outputValue_d = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, output_size * sizeof(outputDataType), 0, 0);
        *outputSample_d = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, outputSample_size * sizeof(unsigned int), 0, 0);
        clQueue->enqueueWriteBuffer(*input_d, CL_FALSE, 0, input->size() * sizeof(inputDataType), reinterpret_cast<void *>(input->data()));
        clQueue->finish();
    }
    catch (cl::Error &err)
    {
        std::cerr << "OpenCL error: " << std::to_string(err.err()) << "." << std::endl;
        throw;
    }
}

void initializeDeviceMemoryD(cl::Context &clContext, cl::CommandQueue *clQueue, std::vector<inputDataType> *input, cl::Buffer *input_d, cl::Buffer *outputValue_d, cl::Buffer *outputStd_d, const uint64_t output_size, cl::Buffer *outputSample_d, const uint64_t outputSample_size)
{
  try
  {
      *input_d = cl::Buffer(clContext, CL_MEM_READ_WRITE, input->size() * sizeof(inputDataType), 0, 0);
      *outputValue_d = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, output_size * sizeof(outputDataType), 0, 0);
      *outputSample_d = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, outputSample_size * sizeof(unsigned int), 0, 0);
      *outputStd_d = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, output_size * sizeof(outputDataType), 0, 0);
      clQueue->enqueueWriteBuffer(*input_d, CL_FALSE, 0, input->size() * sizeof(inputDataType), reinterpret_cast<void *>(input->data()));
      clQueue->finish();
  }
  catch (cl::Error &err)
  {
      std::cerr << "OpenCL error: " << std::to_string(err.err()) << "." << std::endl;
      throw;
  }
}

void initializeDeviceMemoryD(cl::Context &clContext, cl::CommandQueue *clQueue, std::vector<inputDataType> *input, cl::Buffer *input_d, cl::Buffer *outputValue_d, const uint64_t outputSNR_size, cl::Buffer *baselines_d, std::vector<outputDataType> *baselines)
{
    try
    {
        *input_d = cl::Buffer(clContext, CL_MEM_READ_WRITE, input->size() * sizeof(inputDataType), 0, 0);
        *outputValue_d = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, outputSNR_size * sizeof(outputDataType), 0, 0);
        *baselines_d = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, baselines->size() * sizeof(outputDataType), 0, 0);
        clQueue->enqueueWriteBuffer(*input_d, CL_FALSE, 0, input->size() * sizeof(inputDataType), reinterpret_cast<void *>(input->data()));
        clQueue->enqueueWriteBuffer(*baselines_d, CL_FALSE, 0, baselines->size() * sizeof(outputDataType), reinterpret_cast<void *>(baselines->data()));
        clQueue->finish();
    }
    catch (cl::Error &err)
    {
        std::cerr << "OpenCL error: " << std::to_string(err.err()) << "." << std::endl;
        throw;
    }
}

int tune(const bool bestMode, const unsigned int nrIterations, const unsigned int minThreads, const unsigned int maxThreads, const unsigned int maxItems, const unsigned int clPlatformID, const unsigned int clDeviceID, const SNR::DataOrdering ordering, const SNR::Kernel kernelTuned, const unsigned int padding, const AstroData::Observation &observation, SNR::snrConf &conf, const unsigned int medianStep, const float nSigma)
{
    bool reinitializeDeviceMemory = true;
    double bestGBs = 0.0;
    SNR::snrConf bestConf;
    cl::Event event;

    // Initialize OpenCL
    isa::OpenCL::OpenCLRunTime openCLRunTime;

    // Allocate memory
    std::vector<inputDataType> input;
    std::vector<outputDataType> baselines;
    cl::Buffer input_d, outputValue_d, outputSample_d, baselines_d, stdevs_d;

    if (ordering == SNR::DataOrdering::DMsSamples)
    {
        input.resize(observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType)));
        if (kernelTuned == SNR::Kernel::MedianOfMediansAbsoluteDeviation || kernelTuned == SNR::Kernel::AbsoluteDeviation)
        {
            baselines.resize(observation.getNrSynthesizedBeams() * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(outputDataType)));
        }
    }
    else
    {
        input.resize(observation.getNrSynthesizedBeams() * observation.getNrSamplesPerBatch() * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType)));
    }

    srand(time(0));
    for (unsigned int beam = 0; beam < observation.getNrSynthesizedBeams(); beam++)
    {
        if (ordering == SNR::DataOrdering::DMsSamples)
        {
            for (unsigned int subbandDM = 0; subbandDM < observation.getNrDMs(true); subbandDM++)
            {
                for (unsigned int dm = 0; dm < observation.getNrDMs(); dm++)
                {
                    for (unsigned int sample = 0; sample < observation.getNrSamplesPerBatch(); sample++)
                    {
                        input[(beam * observation.getNrDMs(true) * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (dm * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + sample] = static_cast<inputDataType>(rand() % 10);
                    }
                }
            }
        }
        else
        {
            for (unsigned int sample = 0; sample < observation.getNrSamplesPerBatch(); sample++)
            {
                for (unsigned int subbandDM = 0; subbandDM < observation.getNrDMs(true); subbandDM++)
                {
                    for (unsigned int dm = 0; dm < observation.getNrDMs(); dm++)
                    {
                        input[(beam * observation.getNrSamplesPerBatch() * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (sample * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs(false, padding / sizeof(inputDataType))) + dm] = static_cast<inputDataType>(std::rand() % 10);
                    }
                }
            }
        }
    }
    if (kernelTuned == SNR::Kernel::MedianOfMediansAbsoluteDeviation || kernelTuned == SNR::Kernel::AbsoluteDeviation)
    {
        for (unsigned int beam = 0; beam < observation.getNrSynthesizedBeams(); beam++)
        {
            for (unsigned int subbandingDM = 0; subbandingDM < observation.getNrDMs(true); subbandingDM++)
            {
                for (unsigned int dm = 0; dm < observation.getNrDMs(); dm++)
                {
                    baselines.at((beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(outputDataType))) + (subbandingDM * observation.getNrDMs()) + dm) = static_cast<outputDataType>((std::rand() % 10) + 1);
                }
            }
        }
    }

    if (!bestMode)
    {
        std::cout << std::fixed << std::endl;
        std::cout << "# nrBeams nrDMs nrSamples *configuration* GB/s time stdDeviation COV" << std::endl
                  << std::endl;
    }

    for (unsigned int threads = minThreads; threads <= maxThreads;)
    {
        conf.setNrThreadsD0(threads);
        if (ordering == SNR::DataOrdering::DMsSamples)
        {
            threads *= 2;
        }
        else
        {
            threads++;
        }
        for (unsigned int itemsPerThread = 1; itemsPerThread <= maxItems; itemsPerThread++)
        {
            if (kernelTuned == SNR::Kernel::SNR)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    if (((itemsPerThread * 5) + 7) > maxItems)
                    {
                        break;
                    }
                    if ((observation.getNrSamplesPerBatch() % itemsPerThread) != 0)
                    {
                        continue;
                    }
                }
                else
                {
                    if (((itemsPerThread * 5) + 3) > maxItems)
                    {
                        break;
                    }
                    if (observation.getNrDMs() % (itemsPerThread * conf.getNrThreadsD0()) != 0)
                    {
                        continue;
                    }
                }
                conf.setNrItemsD0(itemsPerThread);
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    if ((conf.getNrThreadsD0() * conf.getNrItemsD0()) > observation.getNrSamplesPerBatch())
                    {
                        continue;
                    }
                }
            }
            else if ( kernelTuned == SNR::Kernel::SNRSigmaCut )
            {
                if (((itemsPerThread * 5) + 9) > maxItems)
                {
                    break;
                }
                if ((observation.getNrSamplesPerBatch() % itemsPerThread) != 0)
                {
                    continue;
                }
                conf.setNrItemsD0(itemsPerThread);
                if ((conf.getNrThreadsD0() * conf.getNrItemsD0()) > observation.getNrSamplesPerBatch())
                {
                    continue;
                }
            }
            else if (kernelTuned == SNR::Kernel::Max)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    if (((itemsPerThread * 2) + 2) > maxItems)
                    {
                        break;
                    }
                    if ((observation.getNrSamplesPerBatch() % itemsPerThread) != 0)
                    {
                        continue;
                    }
                }
                conf.setNrItemsD0(itemsPerThread);
                if ((conf.getNrThreadsD0() * conf.getNrItemsD0()) > observation.getNrSamplesPerBatch())
                {
                    continue;
                }
            }
            else if (kernelTuned == SNR::Kernel::MaxStdSigmaCut)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    if (((itemsPerThread * 5) + 9) > maxItems)
                    {
                        break;
                    }
                    if ((observation.getNrSamplesPerBatch() % itemsPerThread) != 0)
                    {
                        continue;
                    }
                }
                conf.setNrItemsD0(itemsPerThread);
                if ((conf.getNrThreadsD0() * conf.getNrItemsD0()) > observation.getNrSamplesPerBatch())
                {
                    continue;
                }
            }
            else if (kernelTuned == SNR::Kernel::AbsoluteDeviation)
            {
                if ((observation.getNrSamplesPerBatch() % itemsPerThread) != 0)
                {
                    continue;
                }
                conf.setNrItemsD0(itemsPerThread);
                if ((conf.getNrThreadsD0() * conf.getNrItemsD0()) > observation.getNrSamplesPerBatch())
                {
                    continue;
                }
            }

            // Generate kernel
            double gbs = 0.0;
            cl::Kernel *kernel;
            isa::utils::Timer timer;
            std::string *code;
            if (kernelTuned == SNR::Kernel::SNR || kernelTuned == SNR::Kernel::Max)
            {
                gbs = isa::utils::giga((observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * observation.getNrSamplesPerBatch() * sizeof(inputDataType)) + (observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * sizeof(outputDataType)) + (observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * sizeof(unsigned int)));
            }
            if ( kernelTuned == SNR::Kernel::SNRSigmaCut )
            {
                gbs = isa::utils::giga((2 * observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * observation.getNrSamplesPerBatch() * sizeof(inputDataType)) + (observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * sizeof(outputDataType)) + (observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * sizeof(unsigned int)));
            }
            if (kernelTuned == SNR::Kernel::MaxStdSigmaCut)
            {
                gbs = isa::utils::giga((observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * observation.getNrSamplesPerBatch() * sizeof(inputDataType) * 2.0) + (observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * sizeof(outputDataType) * 2.0) + (observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * sizeof(unsigned int)));
            }
            else if (kernelTuned == SNR::Kernel::MedianOfMedians)
            {
                gbs = isa::utils::giga((observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * observation.getNrSamplesPerBatch() * sizeof(inputDataType)) + (observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * (observation.getNrSamplesPerBatch() / medianStep) * sizeof(outputDataType)));
            }
            else if (kernelTuned == SNR::Kernel::MedianOfMediansAbsoluteDeviation)
            {
                gbs = isa::utils::giga((observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * observation.getNrSamplesPerBatch() * sizeof(inputDataType)) + (observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * (observation.getNrSamplesPerBatch() / medianStep) * sizeof(outputDataType)) + (observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs() * sizeof(outputDataType)));
            }
            else if (kernelTuned == SNR::Kernel::AbsoluteDeviation)
            {
                gbs = isa::utils::giga((observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * observation.getNrSamplesPerBatch() * sizeof(inputDataType)) + (observation.getNrSynthesizedBeams() * static_cast<uint64_t>(observation.getNrDMs(true) * observation.getNrDMs()) * sizeof(outputDataType)) + (observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs() * sizeof(outputDataType)));
            }

            if (kernelTuned == SNR::Kernel::SNR)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    code = SNR::getSNRDMsSamplesOpenCL<inputDataType>(conf, inputDataName, observation, observation.getNrSamplesPerBatch(), padding);
                }
                else
                {
                    code = SNR::getSNRSamplesDMsOpenCL<inputDataType>(conf, inputDataName, observation, observation.getNrSamplesPerBatch(), padding);
                }
            }
            else if ( kernelTuned == SNR::Kernel::SNRSigmaCut )
            {
                code = SNR::getSNRSigmaCutDMsSamplesOpenCL<inputDataType>(conf, inputDataName, observation, observation.getNrSamplesPerBatch(), padding, nSigma);
            }
            else if (kernelTuned == SNR::Kernel::Max)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    code = SNR::getMaxOpenCL<inputDataType>(conf, ordering, inputDataName, observation, 1, padding);
                }
            }
            else if (kernelTuned == SNR::Kernel::MaxStdSigmaCut)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    code = SNR::getMaxStdSigmaCutOpenCL<inputDataType>(conf, ordering, inputDataName, observation, 1, padding, nSigma);
                }
            }
            else if (kernelTuned == SNR::Kernel::MedianOfMedians)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    code = SNR::getMedianOfMediansOpenCL<inputDataType>(conf, ordering, inputDataName, observation, 1, medianStep, padding);
                }
            }
            else if (kernelTuned == SNR::Kernel::MedianOfMediansAbsoluteDeviation)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    code = SNR::getMedianOfMediansAbsoluteDeviationOpenCL<inputDataType>(conf, ordering, inputDataName, observation, 1, medianStep, padding);
                }
            }
            else if (kernelTuned == SNR::Kernel::AbsoluteDeviation)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    code = SNR::getAbsoluteDeviationOpenCL<inputDataType>(conf, ordering, inputDataName, observation, 1, padding);
                }
            }

            if (reinitializeDeviceMemory)
            {
                isa::OpenCL::initializeOpenCL(clPlatformID, 1, openCLRunTime);
                try
                {
                    if ( kernelTuned == SNR::Kernel::SNR || kernelTuned == SNR::Kernel::SNRSigmaCut || kernelTuned == SNR::Kernel::Max )
                    {
                        initializeDeviceMemoryD(*(openCLRunTime.context), &(openCLRunTime.queues->at(clDeviceID)[0]), &input, &input_d, &outputValue_d, observation.getNrSynthesizedBeams() * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(outputDataType)), &outputSample_d, observation.getNrSynthesizedBeams() * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int)));
                    }
                    else if (kernelTuned == SNR::Kernel::MaxStdSigmaCut)
                    {
                      initializeDeviceMemoryD(*(openCLRunTime.context), &(openCLRunTime.queues->at(clDeviceID)[0]), &input, &input_d, &outputValue_d, &stdevs_d, observation.getNrSynthesizedBeams() * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(outputDataType)), &outputSample_d, observation.getNrSynthesizedBeams() * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int)));
                    }
                    else if (kernelTuned == SNR::Kernel::MedianOfMedians)
                    {
                        initializeDeviceMemoryD(*(openCLRunTime.context), &(openCLRunTime.queues->at(clDeviceID)[0]), &input, &input_d, &outputValue_d, observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs() * isa::utils::pad(observation.getNrSamplesPerBatch() / medianStep, padding / sizeof(outputDataType)));
                    }
                    else if (kernelTuned == SNR::Kernel::MedianOfMediansAbsoluteDeviation)
                    {
                        initializeDeviceMemoryD(*(openCLRunTime.context), &(openCLRunTime.queues->at(clDeviceID)[0]), &input, &input_d, &outputValue_d, observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs() * isa::utils::pad(observation.getNrSamplesPerBatch() / medianStep, padding / sizeof(outputDataType)), &baselines_d, &baselines);
                    }
                    else if (kernelTuned == SNR::Kernel::AbsoluteDeviation)
                    {
                        initializeDeviceMemoryD(*(openCLRunTime.context), &(openCLRunTime.queues->at(clDeviceID)[0]), &input, &input_d, &outputValue_d, observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs() * isa::utils::pad(observation.getNrSamplesPerBatch(), padding / sizeof(outputDataType)), &baselines_d, &baselines);
                    }
                }
                catch (cl::Error &err)
                {
                    return -1;
                }
                reinitializeDeviceMemory = false;
            }
            try
            {
                if (kernelTuned == SNR::Kernel::SNR)
                {
                    if (ordering == SNR::DataOrdering::DMsSamples)
                    {
                        kernel = isa::OpenCL::compile("snrDMsSamples" + std::to_string(observation.getNrSamplesPerBatch()), *code, "-cl-mad-enable -Werror", *(openCLRunTime.context), openCLRunTime.devices->at(clDeviceID));
                    }
                    else
                    {
                        kernel = isa::OpenCL::compile("snrSamplesDMs" + std::to_string(observation.getNrDMs(true) * observation.getNrDMs()), *code, "-cl-mad-enable -Werror", *(openCLRunTime.context), openCLRunTime.devices->at(clDeviceID));
                    }
                }
                else if ( kernelTuned == SNR::Kernel::SNRSigmaCut )
                {
                        kernel = isa::OpenCL::compile("snrSigmaCutDMsSamples" + std::to_string(observation.getNrSamplesPerBatch()), *code, "-cl-mad-enable -Werror", *(openCLRunTime.context), openCLRunTime.devices->at(clDeviceID));
                }
                else if (kernelTuned == SNR::Kernel::Max)
                {
                    if (ordering == SNR::DataOrdering::DMsSamples)
                    {
                        kernel = isa::OpenCL::compile("max_DMsSamples_" + std::to_string(observation.getNrSamplesPerBatch()), *code, "-cl-mad-enable -Werror", *(openCLRunTime.context), openCLRunTime.devices->at(clDeviceID));
                    }
                }
                else if (kernelTuned == SNR::Kernel::MaxStdSigmaCut)
                {
                    if (ordering == SNR::DataOrdering::DMsSamples)
                    {
                        kernel = isa::OpenCL::compile("maxStdSigmaCut_DMsSamples_" + std::to_string(observation.getNrSamplesPerBatch()), *code, "-cl-mad-enable -Werror", *(openCLRunTime.context), openCLRunTime.devices->at(clDeviceID));
                    }
                }
                else if (kernelTuned == SNR::Kernel::MedianOfMedians)
                {
                    if (ordering == SNR::DataOrdering::DMsSamples)
                    {
                        kernel = isa::OpenCL::compile("medianOfMedians_DMsSamples_" + std::to_string(medianStep), *code, "-cl-mad-enable -Werror", *(openCLRunTime.context), openCLRunTime.devices->at(clDeviceID));
                    }
                }
                else if (kernelTuned == SNR::Kernel::MedianOfMediansAbsoluteDeviation)
                {
                    if (ordering == SNR::DataOrdering::DMsSamples)
                    {
                        kernel = isa::OpenCL::compile("medianOfMediansAbsoluteDeviation_DMsSamples_" + std::to_string(medianStep), *code, "-cl-mad-enable -Werror", *(openCLRunTime.context), openCLRunTime.devices->at(clDeviceID));
                    }
                }
                else if (kernelTuned == SNR::Kernel::AbsoluteDeviation)
                {
                    if (ordering == SNR::DataOrdering::DMsSamples)
                    {
                        kernel = isa::OpenCL::compile("absolute_deviation_DMsSamples_" + std::to_string(observation.getNrSamplesPerBatch()), *code, "-cl-mad-enable -Werror", *(openCLRunTime.context), openCLRunTime.devices->at(clDeviceID));
                    }
                }
            }
            catch (isa::OpenCL::OpenCLError &err)
            {
                std::cerr << err.what() << std::endl;
                delete code;
                break;
            }
            delete code;

            cl::NDRange global, local;
            if (kernelTuned == SNR::Kernel::SNR || kernelTuned == SNR::Kernel::SNRSigmaCut || kernelTuned == SNR::Kernel::Max || kernelTuned == SNR::Kernel::MaxStdSigmaCut)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    global = cl::NDRange(conf.getNrThreadsD0(), observation.getNrDMs(true) * observation.getNrDMs(), observation.getNrSynthesizedBeams());
                    local = cl::NDRange(conf.getNrThreadsD0(), 1, 1);
                }
                else
                {
                    global = cl::NDRange((observation.getNrDMs(true) * observation.getNrDMs()) / conf.getNrItemsD0(), observation.getNrSynthesizedBeams());
                    local = cl::NDRange(conf.getNrThreadsD0(), 1);
                }
            }
            else if (kernelTuned == SNR::Kernel::MedianOfMedians || kernelTuned == SNR::Kernel::MedianOfMediansAbsoluteDeviation)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    global = cl::NDRange(conf.getNrThreadsD0() * (observation.getNrSamplesPerBatch() / medianStep), observation.getNrDMs(true) * observation.getNrDMs(), observation.getNrSynthesizedBeams());
                    local = cl::NDRange(conf.getNrThreadsD0(), 1, 1);
                }
            }
            else if (kernelTuned == SNR::Kernel::AbsoluteDeviation)
            {
                if (ordering == SNR::DataOrdering::DMsSamples)
                {
                    global = cl::NDRange(observation.getNrSamplesPerBatch() / conf.getNrItemsD0(), observation.getNrDMs(true) * observation.getNrDMs(), observation.getNrSynthesizedBeams());
                    local = cl::NDRange(conf.getNrThreadsD0(), 1, 1);
                }
            }
            if ( kernelTuned == SNR::Kernel::SNR || kernelTuned == SNR::Kernel::SNRSigmaCut )
            {
                kernel->setArg(0, input_d);
                kernel->setArg(1, outputValue_d);
                kernel->setArg(2, outputSample_d);
            }
            else if (kernelTuned == SNR::Kernel::Max)
            {
                kernel->setArg(0, input_d);
                kernel->setArg(1, outputValue_d);
                kernel->setArg(2, outputSample_d);
            }
            else if (kernelTuned == SNR::Kernel::MaxStdSigmaCut)
            {
                kernel->setArg(0, input_d);
                kernel->setArg(1, outputValue_d);
                kernel->setArg(2, outputSample_d);
                kernel->setArg(3, stdevs_d);
            }
            else if (kernelTuned == SNR::Kernel::MedianOfMedians)
            {
                kernel->setArg(0, input_d);
                kernel->setArg(1, outputValue_d);
            }
            else if (kernelTuned == SNR::Kernel::MedianOfMediansAbsoluteDeviation)
            {
                kernel->setArg(0, baselines_d);
                kernel->setArg(1, input_d);
                kernel->setArg(2, outputValue_d);
            }
            else if (kernelTuned == SNR::Kernel::AbsoluteDeviation)
            {
                kernel->setArg(0, baselines_d);
                kernel->setArg(1, input_d);
                kernel->setArg(2, outputValue_d);
            }
            try
            {
                // Warm-up run
                openCLRunTime.queues->at(clDeviceID)[0].finish();
                openCLRunTime.queues->at(clDeviceID)[0].enqueueNDRangeKernel(*kernel, cl::NullRange, global, local, 0, &event);
                event.wait();
                // Tuning runs
                for (unsigned int iteration = 0; iteration < nrIterations; iteration++)
                {
                    timer.start();
                    openCLRunTime.queues->at(clDeviceID)[0].enqueueNDRangeKernel(*kernel, cl::NullRange, global, local, 0, &event);
                    event.wait();
                    timer.stop();
                }
            }
            catch (cl::Error &err)
            {
                std::cerr << "OpenCL error kernel execution (";
                std::cerr << conf.print();
                std::cerr << "): " << std::to_string(err.err()) << "." << std::endl;
                delete kernel;
                if (err.err() == -4 || err.err() == -61)
                {
                    return -1;
                }
                reinitializeDeviceMemory = true;
                break;
            }
            delete kernel;

            if ((gbs / timer.getAverageTime()) > bestGBs)
            {
                bestGBs = gbs / timer.getAverageTime();
                bestConf = conf;
            }
            if (!bestMode)
            {
                std::cout << observation.getNrSynthesizedBeams() << " " << observation.getNrDMs(true) * observation.getNrDMs() << " " << observation.getNrSamplesPerBatch() << " ";
                std::cout << conf.print() << " ";
                std::cout << std::setprecision(3);
                std::cout << gbs / timer.getAverageTime() << " ";
                std::cout << std::setprecision(6);
                std::cout << timer.getAverageTime() << " " << timer.getStandardDeviation() << " " << timer.getCoefficientOfVariation() << std::endl;
            }
        }
    }

    if (bestMode)
    {
        std::cout << observation.getNrDMs(true) * observation.getNrDMs() << " " << observation.getNrSamplesPerBatch() << " " << bestConf.print() << std::endl;
    }
    else
    {
        std::cout << std::endl;
    }
    return 0;
}
