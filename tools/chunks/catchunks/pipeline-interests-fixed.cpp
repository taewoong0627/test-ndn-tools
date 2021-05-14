/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2016-2019, Regents of the University of California,
 *                          Colorado State University,
 *                          University Pierre & Marie Curie, Sorbonne University.
 *
 * This file is part of ndn-tools (Named Data Networking Essential Tools).
 * See AUTHORS.md for complete list of ndn-tools authors and contributors.
 *
 * ndn-tools is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ndn-tools is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndn-tools, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 *
 * See AUTHORS.md for complete list of ndn-cxx authors and contributors.
 *
 * @author Wentao Shang
 * @author Steve DiBenedetto
 * @author Andrea Tosatto
 * @author Davide Pesavento
 * @author Chavoosh Ghasemi
 */

#include "pipeline-interests-fixed.hpp"
#include "data-fetcher.hpp"

namespace ndn {
namespace chunks {

PipelineInterestsFixed::PipelineInterestsFixed(Face& face, const Options& opts)
  : PipelineInterests(face, opts)
{
  m_segmentFetchers.resize(m_options.maxPipelineSize);

  if (m_options.isVerbose) {
    printOptions();
    std::cerr << "\tPipeline size = " << m_options.maxPipelineSize << "\n";
  }
}

PipelineInterestsFixed::~PipelineInterestsFixed()
{
  cancel();
}

void
PipelineInterestsFixed::doRun()
{
  // if the FinalBlockId is unknown, this could potentially request non-existent segments
  for (size_t nRequestedSegments = 0;
       nRequestedSegments < m_options.maxPipelineSize;
       ++nRequestedSegments) {
    if (!fetchNextSegment(nRequestedSegments))
      // all segments have been requested
      break;
  }
}

bool
PipelineInterestsFixed::fetchNextSegment(std::size_t pipeNo)
{
  if (isStopping())
    return false;

  if (m_hasFailure) {
    onFailure("Fetching terminated but no final segment number has been found");
    return false;
  }

  uint64_t nextSegmentNo = getNextSegmentNo();
  if (m_hasFinalBlockId && nextSegmentNo > m_lastSegmentNo)
    return false;

  // send interest for next segment
  if (m_options.isVerbose)
    std::cerr << "Requesting segment #" << nextSegmentNo << std::endl;

  auto interest = Interest()
                  .setName(Name(m_prefix).appendSegment(nextSegmentNo))
                  .setCanBePrefix(false)
                  .setMustBeFresh(m_options.mustBeFresh)
                  .setInterestLifetime(m_options.interestLifetime);

  auto fetcher = DataFetcher::fetch(m_face, interest,
                                    m_options.maxRetriesOnTimeoutOrNack,
                                    m_options.maxRetriesOnTimeoutOrNack,
                                    bind(&PipelineInterestsFixed::handleData, this, _1, _2, pipeNo),
                                    bind(&PipelineInterestsFixed::handleFail, this, _2, pipeNo),
                                    bind(&PipelineInterestsFixed::handleFail, this, _2, pipeNo),
                                    m_options.isVerbose);

  BOOST_ASSERT(!m_segmentFetchers[pipeNo].first || !m_segmentFetchers[pipeNo].first->isRunning());
  m_segmentFetchers[pipeNo] = make_pair(fetcher, nextSegmentNo);

  return true;
}

void
PipelineInterestsFixed::doCancel()
{
  for (auto& fetcher : m_segmentFetchers) {
    if (fetcher.first)
      fetcher.first->cancel();
  }

  m_segmentFetchers.clear();
}

void
PipelineInterestsFixed::handleData(const Interest& interest, const Data& data, size_t pipeNo)
{
  if (isStopping())
    return;

  BOOST_ASSERT(data.getName().equals(interest.getName()));

  if (m_options.isVerbose)
    std::cerr << "Received segment #" << getSegmentFromPacket(data) << std::endl;

  int segment_no = interest.getName().get(-1).toSegment();
  auto content = data.getContent();
  content.parse();

  if(hash_data.size() != 0 && hash_data.find(segment_no+1)->second != 0) {
    auto cData = hash_data.find(segment_no+1)->second;
    auto m_content = cData->getContent();
    m_content.parse();
    auto hash_block = m_content.get(tlv::SignatureValue);
    if (memcmp((void*)content.get(tlv::SignatureValue).value(), (void*)hash_block.value(), data.getSignatureValue().value_size())) {
      return onFailure("Failure hash key error");
    } else {
      hash_data.erase(segment_no+1);
      onData(*cData);
    }
  } else {
    for (auto iter = content.elements_begin(); iter != content.elements_end(); iter++) {
      if(iter->type() == tlv::SignatureValue) {
        auto signature_block = iter.base();
        std::shared_ptr<Block> block = std::make_shared<Block>(*signature_block); 
        hash_map.insert(std::pair<int, std::shared_ptr<Block>>(segment_no, block));
      }
    }
  }

  if(segment_no != 0){
    auto hash_block = hash_map.find(segment_no-1)->second;
    if(hash_block == 0) {
      std::shared_ptr<Data> m_data = std::make_shared<Data>(data); 
      hash_data.insert(std::pair<int, std::shared_ptr<Data>>(segment_no, m_data));
    } else if (memcmp((void*)data.getSignatureValue().value(), (void*)hash_block->value(), data.getSignatureValue().value_size())) {
      return onFailure("Failure hash key error");
    } else {
      hash_data.erase(segment_no-1);
      onData(data);
    }
  } else {
    onData(data);
  }


  if (!m_hasFinalBlockId && data.getFinalBlock()) {
    m_lastSegmentNo = data.getFinalBlock()->toSegment();
    m_hasFinalBlockId = true;

    for (auto& fetcher : m_segmentFetchers) {
      if (fetcher.first == nullptr)
        continue;

      if (fetcher.second > m_lastSegmentNo) {
        // stop trying to fetch segments that are beyond m_lastSegmentNo
        fetcher.first->cancel();
      }
      else if (fetcher.first->hasError()) { // fetcher.second <= m_lastSegmentNo
        // there was an error while fetching a segment that is part of the content
        return onFailure("Failure retrieving segment #" + to_string(fetcher.second));
      }
    }
  }

  if (allSegmentsReceived()) {
    if (!m_options.isQuiet) {
      printSummary();
    }
  }
  else {
    fetchNextSegment(pipeNo);
  }
}

void PipelineInterestsFixed::handleFail(const std::string& reason, std::size_t pipeNo)
{
  if (isStopping())
    return;

  // if the failed segment is definitely part of the content, raise a fatal error
  if (m_hasFinalBlockId && m_segmentFetchers[pipeNo].second <= m_lastSegmentNo)
    return onFailure(reason);

  if (!m_hasFinalBlockId) {
    bool areAllFetchersStopped = true;
    for (auto& fetcher : m_segmentFetchers) {
      if (fetcher.first == nullptr)
        continue;

      // cancel fetching all segments that follow
      if (fetcher.second > m_segmentFetchers[pipeNo].second) {
        fetcher.first->cancel();
      }
      else if (fetcher.first->isRunning()) { // fetcher.second <= m_segmentFetchers[pipeNo].second
        areAllFetchersStopped = false;
      }
    }

    if (areAllFetchersStopped) {
      onFailure("Fetching terminated but no final segment number has been found");
    }
    else {
      m_hasFailure = true;
    }
  }
}

} // namespace chunks
} // namespace ndn
