/**
 * MetriSCA - A side-channel analysis library
 * Copyright 2021, School of Computer and Communication Sciences, EPFL.
 *
 * All rights reserved. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE.md file.
 */

#include "metrisca/metrics/rank_estimation_metric.hpp"

#include "metrisca/core/logger.hpp"
#include "metrisca/core/errors.hpp"
#include "metrisca/core/csv_writer.hpp"
#include "metrisca/core/trace_dataset.hpp"
#include "metrisca/utils/numerics.hpp"
#include "metrisca/distinguishers/distinguisher.hpp"
#include "metrisca/models/model.hpp"
#include "metrisca/core/lazy_function.hpp"
#include "metrisca/core/indicators.hpp"
#include "metrisca/core/arg_list.hpp"
#include "metrisca/core/parallel.hpp"

#include <limits>
#include <atomic>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <math.h>

#define DOUBLE_NAN (std::numeric_limits<double>::signaling_NaN())

namespace metrisca {

    Result<void, Error> RankEstimationMetric::Init(const ArgumentList& args)
    {
        // First initialize the base plugin
        auto base_result = BasicMetricPlugin::Init(args);
        if(base_result.IsError())
            return base_result.Error();

        // Ensure the dataset has fixed key
        if (m_Dataset->GetHeader().KeyMode != KeyGenerationMode::FIXED) {
            METRISCA_ERROR("RankEstimationMetric require the key to be fixed accross the entire dataset");
            return Error::UNSUPPORTED_OPERATION;
        }

        // Retrieve the key from the dataset
        auto key_span = m_Dataset->GetKey(0);
        m_Key.insert(m_Key.end(), key_span.begin(), key_span.end());

        // Retrieve the bin count argument
        auto bin_count = args.GetUInt32(ARG_NAME_BIN_COUNT);
        m_BinCount = bin_count.value_or(10000);
        

        // Sample & trace count/step
        auto sample_start = args.GetUInt32(ARG_NAME_SAMPLE_START);
        auto sample_end = args.GetUInt32(ARG_NAME_SAMPLE_END);
        auto trace_count = args.GetUInt32(ARG_NAME_TRACE_COUNT);
        auto trace_step = args.GetUInt32(ARG_NAME_TRACE_STEP);
        TraceDatasetHeader header = m_Dataset->GetHeader();
        m_TraceCount = trace_count.value_or(header.NumberOfTraces);
        m_TraceStep = trace_step.value_or(0);
        m_SampleStart = sample_start.value_or(0);
        m_SampleCount = sample_end.value_or(header.NumberOfSamples) - m_SampleStart;

        // Sanity checks
        if(m_SampleCount == 0)
            return Error::INVALID_ARGUMENT;

        if(m_SampleStart + m_SampleCount > header.NumberOfSamples)
            return Error::INVALID_ARGUMENT;

        if(m_TraceCount > header.NumberOfTraces)
            return Error::INVALID_ARGUMENT;

        return  {};
    }

    Result<void, Error> RankEstimationMetric::Compute() 
    {

        // Conveniance constant
        const size_t number_of_traces = m_TraceCount;
        const size_t number_of_samples = m_SampleCount;
        const size_t first_sample = m_SampleStart;
        const size_t last_sample = first_sample + m_SampleCount;

        std::vector<uint32_t> steps = (m_TraceStep > 0) ?
            numerics::ARange(m_TraceStep, m_TraceCount + 1, m_TraceStep) :
            std::vector<uint32_t>{ m_TraceCount };

        // Write CSV header
        CSVWriter writer(m_OutputFile);
        writer << "number_of_traces";
        for (size_t j = 0; j < m_Key.size(); j++) {
            for(uint32_t i = 0; i < 256; ++i) {
                writer << "key_byte_" + std::to_string(j) + "@" + std::to_string(i);
            }
        }
        writer << csv::EndRow;

        // Retrieve key probabilities for each step and each bytes
        METRISCA_INFO("Retrieving key probabilities");
        std::vector<std::vector<std::array<double, 256>>> keyProbabilities; // [step][byte][byteValue]
        keyProbabilities.resize(steps.size());
        for (auto& elem : keyProbabilities) elem.resize(m_Key.size());

        std::atomic_bool isError = false;
        Error error;
        indicators::ProgressBar progressBar{
            indicators::option::BarWidth{50},
            indicators::option::MaxProgress{ steps.size() * m_Key.size() },
            indicators::option::PrefixText{ "Computing probabilities " },
            indicators::option::ShowElapsedTime{ true },
            indicators::option::ShowRemainingTime{ true },
            indicators::option::ShowPercentage{ true }
        };
        progressBar.set_progress(0);

        metrisca::parallel_for(0, steps.size() * m_Key.size(), [&](size_t first, size_t last, bool is_main_thread) {
            for (size_t idx = first; idx != last; idx++) {
                if (isError) return; 
                size_t keyByteIdx = idx % m_Key.size();
                size_t stepIdx = idx / m_Key.size();

                auto result = ComputeProbabilities(steps[stepIdx], keyByteIdx);
                if (result.IsError()) {
                    METRISCA_ERROR("Fail to compute probabilities with {} traces (key-byte {})", steps[stepIdx], keyByteIdx);
                    isError = true;
                    error = result.Error();
                    return;
                }
                keyProbabilities[stepIdx][keyByteIdx] = result.Value();

                {
                    std::lock_guard guard(m_GlobalLock);
                    progressBar.tick();
                    progressBar.set_option(indicators::option::PostfixText{ std::to_string(progressBar.current()) + "/" + std::to_string(steps.size() * m_Key.size()) + " " });
                }
            }
        });
        progressBar.set_progress(steps.size() * m_Key.size());
        progressBar.set_option(indicators::option::PostfixText{ "  Completed  " });
        progressBar.mark_as_completed();



        // Output all probabilities to the output file
        for (size_t stepIdx = 0; stepIdx != steps.size(); ++stepIdx)
        {
            writer << steps[stepIdx];
            for (size_t keyByteIdx = 0; keyByteIdx != m_Key.size(); ++keyByteIdx)
            {
                for (size_t keyValue = 0; keyValue != 256; keyValue++)
                {
                    writer << keyProbabilities[stepIdx][keyByteIdx][keyValue];
                }
            }
            writer << csv::EndRow;
        }

        // Do some stuff with things that make more stuff togethers
        METRISCA_INFO("Computing histogram in order to approximate the rank of the whole key within our model");

        // Return success of the operatrion
        return {};
    }

    Result<std::array<double, 256>, Error> RankEstimationMetric::ComputeProbabilities(size_t number_of_traces, size_t keyByteIdx)
    {
        // Result of all of this shenanigans
        std::array<double, 256> probabilities;
        probabilities.fill(0.0);

        // Utility variables
        const size_t number_of_samples = m_SampleCount;
        const size_t first_sample = m_SampleStart;
        const size_t last_sample = first_sample + m_SampleCount;

        // Compute the modelization matrix for the current byte
        //TODO: Move this to the parent function in order to prevent it from
        //      being recomputed at every single step.// For each group/step compute the covariance matrix between each sample
        Matrix<int32_t> models;
        {
            std::lock_guard<std::mutex> guard(m_GlobalLock);
            m_Distinguisher->GetPowerModel()->SetByteIndex(keyByteIdx);
            auto result = m_Distinguisher->GetPowerModel()->Model();
            if (result.IsError()) {
                METRISCA_ERROR("Fail to compute the model for KeyByte {}", keyByteIdx);
                return result.Error();
            }
            models = result.Value();
        }


        // Using our prior knowledge of the correct key, group each 
        // traces by their "expected" output using the model.
        // Notice that in the scenario where we do not know the key, we can simply do this for each 
        // possible key hypothesis.
        //TODO: Same here 
        std::array<std::vector<size_t>, 256> grouped_by_expected_result; // Only store indices of the traces (to save memory)
        std::unordered_set<size_t> group_without_model;

        for (size_t i = 0; i != number_of_traces; ++i) {
            int32_t expected_output = models(m_Key[keyByteIdx], i);

            if (expected_output < 0 || expected_output >= 256) {
                METRISCA_ERROR("Currently only model producing byte (in range 0 .. 255) are supported by this metric. Instead got {}", expected_output);
                return Error::UNSUPPORTED_OPERATION;
            }

            grouped_by_expected_result[expected_output].push_back(i);
        }

        for (size_t groupIdx = 0; groupIdx != 256; groupIdx++) {
            if (grouped_by_expected_result[groupIdx].empty()) {
                group_without_model.insert(groupIdx);
            }
        }

        // For each group, for each sample, computes the average within the group
        std::array<std::vector<double>, 256> group_average; // [expected result = 256][sample]

        for (size_t groupIdx = 0, iter = 0; groupIdx != 256; ++groupIdx) { // For each of the possible output
            group_average[groupIdx].resize(number_of_samples, 0.0);
            size_t matching_trace_count = 0;

            for (size_t traceIdx : grouped_by_expected_result[groupIdx]) {
                // Ignore all traces outside of the sweet zone
                if (traceIdx >= number_of_traces) continue;
                matching_trace_count += 1;

                // Computing the average for this specific group
                for (size_t sampleIdx = first_sample; sampleIdx != last_sample; sampleIdx++) {
                    group_average[groupIdx][sampleIdx - first_sample] += m_Dataset->GetSample(sampleIdx)[traceIdx];
                }
            }

            // If no such traces exists
            if (matching_trace_count == 0) {
                for (size_t j = 0; j < number_of_samples; j++) {
                    group_average[groupIdx][j] = DOUBLE_NAN;
                }
            }
            else {
                for (size_t j = 0; j < number_of_samples; j++) {
                    group_average[groupIdx][j] /= matching_trace_count;  
                }
            }
        }

        // Reducing number of samples to speed-up the computation
        // std::unordered_map<size_t, double> cumulated_diff;
        std::vector<size_t> selected_sample;
        // for (size_t k = m_SampleStart; k != m_SampleCount; ++k) {
        //     cumulated_diff[k] = 0.0;  
        // }

        for (size_t i = 0; i != 256; i++) {
            // Skip group without model
            if (group_without_model.find(i) != group_without_model.end()) continue;

            for (size_t j = i + 1; j != 256; ++j) {
                if (group_without_model.find(j) != group_without_model.end()) continue;

                // Iterate over all available sample
                for (size_t k = m_SampleStart; k != m_SampleCount; ++k) {
                    double diff = (group_average[i][k - first_sample] - group_average[j][k - first_sample]) * 
                                  (group_average[i][k - first_sample] - group_average[j][k - first_sample]);
                    // cumulated_diff[k] += diff;
                    if (diff > /* 50.0 */ 0.0 && std::find(selected_sample.begin(), selected_sample.end(), k) == selected_sample.end()) {
                        selected_sample.push_back(k);
                    }
                }
            }
        }
        size_t const reduced_sample_number = selected_sample.size();     

        // Compute the covariance matrix
        Matrix<double> cov_matrix(reduced_sample_number, reduced_sample_number);

        for (size_t i = 0; i != reduced_sample_number; ++i) {
            cov_matrix.FillRow(i, 0.0);
        }

        for (size_t groupIdx = 0; groupIdx != 256; ++groupIdx)
        {
            for (size_t traceIdx : grouped_by_expected_result[groupIdx]) {
                for (size_t row = 0; row != reduced_sample_number; row++) {
                    for (size_t col = 0; col != reduced_sample_number; col++) {
                        cov_matrix(row, col) += (m_Dataset->GetSample(selected_sample[row])[traceIdx] - group_average[groupIdx][selected_sample[row] - first_sample]) *
                                                (m_Dataset->GetSample(selected_sample[col])[traceIdx] - group_average[groupIdx][selected_sample[col] - first_sample]);
                    }
                }
            }
        }

        for (size_t row = 0; row != reduced_sample_number; row++) {
            for (size_t col = 0; col != reduced_sample_number; col++) {
                cov_matrix(row, col) /= (number_of_traces - 1);
            }
        }

        Matrix<double> cov_inverse_matrix = cov_matrix.CholeskyInverse();

        // Finally compute the probabilities
        std::vector<double> noise_vector, intermediary_result; // number of samples
        noise_vector.resize(reduced_sample_number);
        intermediary_result.resize(reduced_sample_number);

        for (size_t groupIdx = 0; groupIdx != 256; groupIdx++)
        {
            if (group_without_model.find(groupIdx) != group_without_model.end()) continue;

            for (size_t traceIdx = 0; traceIdx != number_of_traces; ++traceIdx)
            {
                int32_t expected_output = models(groupIdx, traceIdx);
                if (group_without_model.find(expected_output) != group_without_model.end()) continue;

                for (size_t j = 0; j != reduced_sample_number; ++j)
                {
                    noise_vector[j] = m_Dataset->GetSample(selected_sample[j])[traceIdx] - group_average[expected_output][selected_sample[j] - first_sample];
                }

                // Compute noise transposed * inverse
                for (size_t i = 0; i < reduced_sample_number; i++) {
                    double sum = 0.0;
                    for (size_t j = 0; j < reduced_sample_number; j++) {
                        sum += noise_vector[j] * cov_inverse_matrix/* [expected_output] */(j, i); // inverse of covariace matrix (symmetric) is also symmetric so the order doesn't matter
                    }
                    intermediary_result[i] = sum;
                }

                // compute intermediary_result * noise
                double final_result = 0;
                for (size_t i = 0; i < reduced_sample_number; i++) {
                    final_result += intermediary_result[i] * noise_vector[i];
                }

                // finally update the probability
                probabilities[groupIdx] += -0.5 * final_result;
            }
        }

        // Finally upon the success of the current function, return the underlying probabilities
        return probabilities;
    }

}
