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
#include <ctime>

#include <configuration.hpp>

#include <ArgumentList.hpp>
#include <Observation.hpp>
#include <InitializeOpenCL.hpp>
#include <Kernel.hpp>
#include <utils.hpp>
#include <SNR.hpp>
#include <Statistics.hpp>

int test(const bool printResults, const bool printCode, const unsigned int clPlatformID, const unsigned int clDeviceID, const SNR::DataOrdering ordering, const SNR::Kernel kernelUnderTest, const unsigned int padding, const AstroData::Observation &observation, SNR::snrConf &conf);

int main(int argc, char *argv[])
{
    bool printCode = false;
    bool printResults = false;
    int returnCode = 0;
    unsigned int padding = 0;
    unsigned int clPlatformID = 0;
    unsigned int clDeviceID = 0;
    SNR::Kernel kernel;
    SNR::DataOrdering ordering;
    AstroData::Observation observation;
    SNR::snrConf conf;

    try
    {
        isa::utils::ArgumentList args(argc, argv);
        if (args.getSwitch("-snr"))
        {
            kernel = SNR::Kernel::SNR;
        }
        else if (args.getSwitch("-max"))
        {
            kernel = SNR::Kernel::Max;
        }
        else{
            std::cerr << "One switch between -snr and -max is required." << std::endl;
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
        printCode = args.getSwitch("-print_code");
        printResults = args.getSwitch("-print_results");
        clPlatformID = args.getSwitchArgument<unsigned int>("-opencl_platform");
        clDeviceID = args.getSwitchArgument<unsigned int>("-opencl_device");
        padding = args.getSwitchArgument<unsigned int>("-padding");
        conf.setNrThreadsD0(args.getSwitchArgument<unsigned int>("-threadsD0"));
        conf.setNrItemsD0(args.getSwitchArgument<unsigned int>("-itemsD0"));
        conf.setSubbandDedispersion(args.getSwitch("-subband"));
        observation.setNrSynthesizedBeams(args.getSwitchArgument<unsigned int>("-beams"));
        if (conf.getSubbandDedispersion())
        {
            observation.setNrSamplesPerBatch(args.getSwitchArgument<unsigned int>("-samples"), true);
            observation.setDMRange(args.getSwitchArgument<unsigned int>("-subbanding_dms"), 0.0f, 0.0f, true);
        }
        else
        {
            observation.setNrSamplesPerBatch(args.getSwitchArgument<unsigned int>("-samples"));
            observation.setDMRange(1, 0.0f, 0.0f, true);
        }
        observation.setDMRange(args.getSwitchArgument<unsigned int>("-dms"), 0.0, 0.0);
    }
    catch (isa::utils::SwitchNotFound &err)
    {
        std::cerr << err.what() << std::endl;
        return 1;
    }
    catch (std::exception &err)
    {
        std::cerr << "Usage: " << argv[0] << " [-snr | -max] [-dms_samples | -samples_dms] [-print_code] [-print_results] -opencl_platform ... -opencl_device ... -padding ... -threadsD0 ... -itemsD0 ... [-subband] -beams ... -dms ... -samples ..." << std::endl;
        std::cerr << "\t -subband : -subbanding_dms ..." << std::endl;
        return 1;
    }
    returnCode = test(printResults, printCode, clPlatformID, clDeviceID, ordering, kernel, padding, observation, conf);

    return returnCode;
}

int test(const bool printResults, const bool printCode, const unsigned int clPlatformID, const unsigned int clDeviceID, const SNR::DataOrdering ordering, const SNR::Kernel kernelUnderTest, const unsigned int padding, const AstroData::Observation &observation, SNR::snrConf &conf)
{
    uint64_t wrongSamples = 0;
    uint64_t wrongPositions = 0;

    // Initialize OpenCL
    cl::Context *clContext = new cl::Context();
    std::vector<cl::Platform> *clPlatforms = new std::vector<cl::Platform>();
    std::vector<cl::Device> *clDevices = new std::vector<cl::Device>();
    std::vector<std::vector<cl::CommandQueue>> *clQueues = new std::vector<std::vector<cl::CommandQueue>>();
    isa::OpenCL::initializeOpenCL(clPlatformID, 1, clPlatforms, clContext, clDevices, clQueues);

    // Allocate memory
    std::vector<inputDataType> input;
    std::vector<float> output;
    std::vector<unsigned int> outputSampleSNR;
    cl::Buffer input_d, output_d, outputSampleSNR_d;

    if (ordering == SNR::DataOrdering::DMsSamples)
    {
        input.resize(observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType)));
    }
    else
    {
        input.resize(observation.getNrSynthesizedBeams() * observation.getNrSamplesPerBatch() * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType)));
    }
    output.resize(observation.getNrSynthesizedBeams() * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(float)));
    if (kernelUnderTest == SNR::Kernel::SNR)
    {
        outputSampleSNR.resize(observation.getNrSynthesizedBeams() * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int)));
    }
    try
    {
        input_d = cl::Buffer(*clContext, CL_MEM_READ_WRITE, input.size() * sizeof(inputDataType), 0, 0);
        output_d = cl::Buffer(*clContext, CL_MEM_WRITE_ONLY, output.size() * sizeof(float), 0, 0);
        if (kernelUnderTest == SNR::Kernel::SNR)
        {
            outputSampleSNR_d = cl::Buffer(*clContext, CL_MEM_WRITE_ONLY, outputSampleSNR.size() * sizeof(unsigned int), 0, 0);
        }
    }
    catch (cl::Error &err)
    {
        std::cerr << "OpenCL error allocating memory: " << std::to_string(err.err()) << "." << std::endl;
        return 1;
    }

    // Generate test data
    std::vector<unsigned int> maxSample(observation.getNrSynthesizedBeams() * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int)));

    srand(time(0));
    for (auto item = maxSample.begin(); item != maxSample.end(); ++item)
    {
        *item = rand() % observation.getNrSamplesPerBatch();
    }
    for (unsigned int beam = 0; beam < observation.getNrSynthesizedBeams(); beam++)
    {
        if (printResults)
        {
            std::cout << "Beam: " << beam << std::endl;
        }
        if (ordering == SNR::DataOrdering::DMsSamples)
        {
            for (unsigned int subbandDM = 0; subbandDM < observation.getNrDMs(true); subbandDM++)
            {
                for (unsigned int dm = 0; dm < observation.getNrDMs(); dm++)
                {
                    if (printResults)
                    {
                        std::cout << "DM: " << (subbandDM * observation.getNrDMs()) + dm << " -- ";
                    }
                    for (unsigned int sample = 0; sample < observation.getNrSamplesPerBatch(); sample++)
                    {
                        if (sample == maxSample.at((beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int))) + (subbandDM * observation.getNrDMs()) + dm))
                        {
                            input[(beam * observation.getNrDMs(true) * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (dm * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + sample] = static_cast<inputDataType>(10 + (rand() % 10));
                        }
                        else
                        {
                            input[(beam * observation.getNrDMs(true) * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (dm * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + sample] = static_cast<inputDataType>(rand() % 10);
                        }
                        if (printResults)
                        {
                            std::cout << input[(beam * observation.getNrDMs(true) * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (dm * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + sample] << " ";
                        }
                    }
                    if (printResults)
                    {
                        std::cout << std::endl;
                    }
                }
            }
        }
        else
        {
            for (unsigned int sample = 0; sample < observation.getNrSamplesPerBatch(); sample++)
            {
                if (printResults)
                {
                    std::cout << "Sample: " << sample << " -- ";
                }
                for (unsigned int subbandDM = 0; subbandDM < observation.getNrDMs(true); subbandDM++)
                {
                    for (unsigned int dm = 0; dm < observation.getNrDMs(); dm++)
                    {
                        if (sample == maxSample.at((beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int))) + (subbandDM * observation.getNrDMs()) + dm))
                        {
                            input[(beam * observation.getNrSamplesPerBatch() * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (sample * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs(false, padding / sizeof(inputDataType))) + dm] = static_cast<inputDataType>(10 + (rand() % 10));
                        }
                        else
                        {
                            input[(beam * observation.getNrSamplesPerBatch() * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (sample * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs(false, padding / sizeof(inputDataType))) + dm] = static_cast<inputDataType>(rand() % 10);
                        }
                        if (printResults)
                        {
                            std::cout << input[(beam * observation.getNrSamplesPerBatch() * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (sample * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs(false, padding / sizeof(inputDataType))) + dm] << " ";
                        }
                    }
                    if (printResults)
                    {
                        std::cout << std::endl;
                    }
                }
            }
        }
    }
    if (printResults)
    {
        std::cout << std::endl;
    }

    // Copy data structures to device
    try
    {
        clQueues->at(clDeviceID)[0].enqueueWriteBuffer(input_d, CL_FALSE, 0, input.size() * sizeof(inputDataType), reinterpret_cast<void *>(input.data()));
    }
    catch (cl::Error &err)
    {
        std::cerr << "OpenCL error H2D transfer: " << std::to_string(err.err()) << "." << std::endl;
        return 1;
    }

    // Generate kernel
    cl::Kernel *kernel;
    std::string *code;
    if (kernelUnderTest == SNR::Kernel::SNR)
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
    else
    {
        code = SNR::getMaxOpenCL<inputDataType>(conf, ordering, inputDataName, observation, 1, padding);
    }
    if (printCode)
    {
        std::cout << *code << std::endl;
    }

    try
    {
        if (kernelUnderTest == SNR::Kernel::SNR)
        {
            if (ordering == SNR::DataOrdering::DMsSamples)
            {
                kernel = isa::OpenCL::compile("snrDMsSamples" + std::to_string(observation.getNrSamplesPerBatch()), *code, "-cl-mad-enable -Werror", *clContext, clDevices->at(clDeviceID));
            }
            else
            {
                kernel = isa::OpenCL::compile("snrSamplesDMs" + std::to_string(observation.getNrDMs(true) * observation.getNrDMs()), *code, "-cl-mad-enable -Werror", *clContext, clDevices->at(clDeviceID));
            }
        }
        else
        {
            if (ordering == SNR::DataOrdering::DMsSamples)
            {
                kernel = isa::OpenCL::compile("getMax_DMsSamples_" + std::to_string(observation.getNrSamplesPerBatch()), *code, "-cl-mad-enable -Werror", *clContext, clDevices->at(clDeviceID));
            }
        }
    }
    catch (isa::OpenCL::OpenCLError &err)
    {
        std::cerr << err.what() << std::endl;
        return 1;
    }

    // Run OpenCL kernel and CPU control
    std::vector<isa::utils::Statistics<inputDataType>> control(observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs());
    try
    {
        cl::NDRange global;
        cl::NDRange local;

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

        if (kernelUnderTest == SNR::Kernel::SNR)
        {
            kernel->setArg(0, input_d);
            kernel->setArg(1, output_d);
            kernel->setArg(2, outputSampleSNR_d);
        }
        else
        {
            kernel->setArg(0, input_d);
            kernel->setArg(1, output_d);
        }

        clQueues->at(clDeviceID)[0].enqueueNDRangeKernel(*kernel, cl::NullRange, global, local, 0, 0);
        clQueues->at(clDeviceID)[0].enqueueReadBuffer(output_d, CL_TRUE, 0, output.size() * sizeof(float), reinterpret_cast<void *>(output.data()));
        if (kernelUnderTest == SNR::Kernel::SNR)
        {
            clQueues->at(clDeviceID)[0].enqueueReadBuffer(outputSampleSNR_d, CL_TRUE, 0, outputSampleSNR.size() * sizeof(unsigned int), reinterpret_cast<void *>(outputSampleSNR.data()));
        }
    }
    catch (cl::Error &err)
    {
        std::cerr << "OpenCL error: " << std::to_string(err.err()) << "." << std::endl;
        return 1;
    }
    if (kernelUnderTest == SNR::Kernel::SNR)
    {
        for (unsigned int beam = 0; beam < observation.getNrSynthesizedBeams(); beam++)
        {
            for (unsigned int subbandDM = 0; subbandDM < observation.getNrDMs(true); subbandDM++)
            {
                for (unsigned int dm = 0; dm < observation.getNrDMs(); dm++)
                {
                    control[(beam * observation.getNrDMs(true) * observation.getNrDMs()) + (subbandDM * observation.getNrDMs()) + dm] = isa::utils::Statistics<inputDataType>();
                }
            }
            if (ordering == SNR::DataOrdering::DMsSamples)
            {
                for (unsigned int subbandDM = 0; subbandDM < observation.getNrDMs(true); subbandDM++)
                {
                    for (unsigned int dm = 0; dm < observation.getNrDMs(); dm++)
                    {
                        for (unsigned int sample = 0; sample < observation.getNrSamplesPerBatch(); sample++)
                        {
                            control[(beam * observation.getNrDMs(true) * observation.getNrDMs()) + (subbandDM * observation.getNrDMs()) + dm].addElement(input[(beam * observation.getNrDMs(true) * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (dm * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + sample]);
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
                            control[(beam * observation.getNrDMs(true) * observation.getNrDMs()) + (subbandDM * observation.getNrDMs()) + dm].addElement(input[(beam * observation.getNrSamplesPerBatch() * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (sample * observation.getNrDMs(true) * observation.getNrDMs(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs(false, padding / sizeof(inputDataType))) + dm]);
                        }
                    }
                }
            }
        }
    }

    for (unsigned int beam = 0; beam < observation.getNrSynthesizedBeams(); beam++)
    {
        for (unsigned int subbandDM = 0; subbandDM < observation.getNrDMs(true); subbandDM++)
        {
            for (unsigned int dm = 0; dm < observation.getNrDMs(); dm++)
            {
                if (kernelUnderTest == SNR::Kernel::SNR)
                {
                    if (!isa::utils::same(output[(beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(float))) + (subbandDM * observation.getNrDMs()) + dm], static_cast<float>((control[(beam * observation.getNrDMs(true) * observation.getNrDMs()) + (subbandDM * observation.getNrDMs()) + dm].getMax() - control[(beam * observation.getNrDMs(true) * observation.getNrDMs()) + (subbandDM * observation.getNrDMs()) + dm].getMean()) / control[(beam * observation.getNrDMs(true) * observation.getNrDMs()) + (subbandDM * observation.getNrDMs()) + dm].getStandardDeviation()), static_cast<float>(1e-2)))
                    {
                        wrongSamples++;
                    }
                    if (outputSampleSNR.at((beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int))) + (subbandDM * observation.getNrDMs()) + dm) != maxSample.at((beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int))) + (subbandDM * observation.getNrDMs()) + dm))
                    {
                        wrongPositions++;
                    }
                }
                else
                {
                    if (!isa::utils::same(output[(beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(float))) + (subbandDM * observation.getNrDMs()) + dm], static_cast<float>(input[(beam * observation.getNrDMs(true) * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (subbandDM * observation.getNrDMs() * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + (dm * observation.getNrSamplesPerBatch(false, padding / sizeof(inputDataType))) + maxSample.at((beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int))) + (subbandDM * observation.getNrDMs()) + dm)]), static_cast<float>(1e-2)))
                    {
                        wrongSamples++;
                    }
                }
            }
        }
    }

    if (printResults)
    {
        for (unsigned int beam = 0; beam < observation.getNrSynthesizedBeams(); beam++)
        {
            std::cout << "Beam: " << beam << std::endl;
            for (unsigned int subbandDM = 0; subbandDM < observation.getNrDMs(true); subbandDM++)
            {
                for (unsigned int dm = 0; dm < observation.getNrDMs(); dm++)
                {
                    std::cout << output[(beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(float))) + (subbandDM * observation.getNrDMs()) + dm] << "," << (control[(beam * observation.getNrDMs(true) * observation.getNrDMs()) + (subbandDM * observation.getNrDMs()) + dm].getMax() - control[(beam * observation.getNrDMs(true) * observation.getNrDMs()) + (subbandDM * observation.getNrDMs()) + dm].getMean()) / control[(beam * observation.getNrDMs(true) * observation.getNrDMs()) + (subbandDM * observation.getNrDMs()) + dm].getStandardDeviation() << " ; ";
                    if (kernelUnderTest == SNR::Kernel::SNR)
                    {
                        std::cout << outputSampleSNR.at((beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int))) + (subbandDM * observation.getNrDMs()) + dm) << "," << maxSample.at((beam * isa::utils::pad(observation.getNrDMs(true) * observation.getNrDMs(), padding / sizeof(unsigned int))) + (subbandDM * observation.getNrDMs()) + dm) << "  ";
                    }
                }
                std::cout << std::endl;
            }
        }
        std::cout << std::endl;
    }

    if (wrongSamples > 0)
    {
        std::cout << "Wrong samples: " << wrongSamples << " (" << (wrongSamples * 100.0) / static_cast<uint64_t>(observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs()) << "%)." << std::endl;
    }
    else if (wrongPositions > 0)
    {
        std::cout << "Wrong positions: " << wrongPositions << " (" << (wrongPositions * 100.0) / static_cast<uint64_t>(observation.getNrSynthesizedBeams() * observation.getNrDMs(true) * observation.getNrDMs()) << "%)." << std::endl;
    }
    else
    {
        std::cout << "TEST PASSED." << std::endl;
    }
    return 0;
}
