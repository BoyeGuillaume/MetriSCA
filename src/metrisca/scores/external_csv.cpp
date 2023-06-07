/**
 * MetriSCA - A side-channel analysis library
 * Copyright 2021, School of Computer and Communication Sciences, EPFL.
 *
 * All rights reserved. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE.md file.
 */

#include "metrisca/scores/external_csv.hpp"

#include "metrisca/core/logger.hpp"
#include "metrisca/core/trace_dataset.hpp"
#include "metrisca/core/indicators.hpp"
#include "metrisca/models/model.hpp"
#include "metrisca/utils/numerics.hpp"

#include <fstream>
#include <filesystem>
#include <sstream>

namespace metrisca {

    Result<void, Error> ExternalScorePlugin::Init(const ArgumentList& args)
    {
        // Initialize base plugin
        {
            auto result = ScorePlugin::Init(args);
            if (result.IsError())
                return result.Error();
        }

        // Retrieve input file
        auto filename_opt = args.GetString(ARG_NAME_INPUT_FILE);
        if (!filename_opt.has_value()) {
            return Error::INVALID_ARGUMENT;
        }

        m_Filename = filename_opt.value();

        // Check if the file exists
        if (!std::filesystem::exists(m_Filename)) {
            METRISCA_ERROR("Input file {} does not exist", m_Filename);
            return Error::INVALID_ARGUMENT;
        }

        // Return success
        return {};
    }


    Result<std::vector<std::pair<uint32_t, std::vector<std::array<double, 256>>>>, Error>
    ExternalScorePlugin::ComputeScores()
    {
        // Conveniance aliases
        const size_t byteCount = m_Dataset->GetHeader().KeySize;
        const size_t traceCount = m_Dataset->GetHeader().NumberOfTraces;

        // Enumerate each steps (number of traces)
        std::vector<uint32_t> steps = (m_TraceStep > 0) ?
            numerics::ARange(m_TraceStep, m_TraceCount + 1, m_TraceStep) :
            std::vector<uint32_t>{ m_TraceCount };

        // Secondly open the input file as csv
        std::ifstream input(m_Filename);
        input.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        if (!input.is_open()) {
            METRISCA_ERROR("Failed to open input file {}", m_Filename);
            return Error::IO_FAILURE;
        }

        // Indicators
        indicators::ProgressBar progress_bar {
            indicators::option::BarWidth{ 50 },
            indicators::option::PrefixText{ "Parsing input file " },
            indicators::option::ShowElapsedTime{ true },
            indicators::option::ShowRemainingTime{ true },
            indicators::option::ShowPercentage{ true },
            indicators::option::MaxProgress{ byteCount * traceCount }
        };
        
        // Start parsing the file
        std::string line;
        std::vector<std::vector<std::array<double, 256>>> probabilities;
        probabilities.resize(byteCount);

        for (size_t byteIdx = 0; byteIdx != byteCount; byteIdx++) {
            probabilities[byteIdx].resize(traceCount);

            for (size_t traceIdx = 0; traceIdx != traceCount; traceIdx++) {
                if ((byteIdx * traceIdx) % ((traceCount * byteCount) / 1000) == 0) {
                    progress_bar.set_progress(byteIdx * traceCount + traceIdx);
                }

                // Read a line
                std::getline(input, line);

                std::stringstream stream(line);
                std::string entry;

                for (size_t value = 0; value != 256; value++)
                {
                    std::getline(stream, entry, ',');
                    probabilities[byteIdx][traceIdx][value] = std::stod(entry);
                }   
            }
        }

        // Finally complete the progress bar
        progress_bar.set_progress(byteCount * traceCount);
        progress_bar.mark_as_completed();

        // For each key-byte, each key value
        std::vector<std::pair<uint32_t, std::vector<std::array<double, 256>>>> scores;
        scores.reserve(steps.size());
        
        // For each step computes the score
        for (auto traceCount : steps) {
            std::vector<std::array<double, 256>> pScore(byteCount);

            for (size_t byteIdx = 0; byteIdx != byteCount; byteIdx++) {
                for (size_t value = 0; value != 256; value++)
                {
                    // Compute the average of probabilities
                    double average = 0.0;
                    for (size_t traceIdx = 0; traceIdx != traceCount; traceIdx++) {
                        average += probabilities[byteIdx][traceIdx][value];
                    }
                    pScore[byteIdx][value] = average / traceCount;
                }
            }

            scores.push_back(std::make_pair(traceCount, pScore));
        }

        // Return success
        return scores;
    }

}

