/*
 * This file is part of ePipe
 * Copyright (C) 2019, Logical Clocks AB. All rights reserved
 *
 * ePipe is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ePipe is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef NDBDATAREADERS_H
#define NDBDATAREADERS_H

#include "NdbDataReader.h"
#include "ConcurrentPriorityQueue.h"
#include <boost/atomic.hpp>

typedef boost::atomic<Uint64> AtomicLong;

template<typename Keys>
struct BulkIndexComparator {
  
  bool operator()(const Bulk<Keys> &r1, const Bulk<Keys> &r2) const {
    return r1.mProcessingIndex > r2.mProcessingIndex;
  }
};

template<typename Data, typename Conn, typename Keys> 
class NdbDataReaders : public DataReaderOutHandler<Keys>{
public:
  typedef std::vector<NdbDataReader<Data, Conn, Keys>* > DataReadersVec;
  typedef typename DataReadersVec::size_type drvec_size_type;
  NdbDataReaders(TimedRestBatcher<Keys>* elastic);
  void start();
  void processBatch(std::vector<Data>* data_batch);
  void writeOutput(Bulk<Keys> out);
  virtual ~NdbDataReaders();
  
private:
  TimedRestBatcher<Keys>* timedRestBatcher;
  bool mStarted;
  boost::thread mThread;
  
  ConcurrentQueue<std::vector<Data>*>* mBatchedQueue;
  ConcurrentPriorityQueue<Bulk<Keys>, BulkIndexComparator<Keys> >* mWaitingOutQueue;
  
  AtomicLong mLastSent;
  AtomicLong mCurrIndex;
  drvec_size_type mRoundRobinDrIndex;
  
  void run();
  void processWaiting();
  
protected:
  std::vector<NdbDataReader<Data, Conn, Keys>* > mDataReaders;
};

template<typename Data, typename Conn, typename Keys>
NdbDataReaders<Data, Conn, Keys>::NdbDataReaders(TimedRestBatcher<Keys>* batcher) : timedRestBatcher(batcher) {
  mStarted = false;
  mBatchedQueue = new ConcurrentQueue<std::vector<Data>*>();
  mWaitingOutQueue = new ConcurrentPriorityQueue<Bulk<Keys>, BulkIndexComparator<Keys> >();
  mLastSent = 0; 
  mCurrIndex = 0;
  mRoundRobinDrIndex = -1;
}

template<typename Data, typename Conn, typename Keys>
void NdbDataReaders<Data, Conn, Keys>::start() {
  if (mStarted) {
    return;
  }
  
  mThread = boost::thread(&NdbDataReaders::run, this);
  mStarted = true;
}


template<typename Data, typename Conn, typename Keys>
void NdbDataReaders<Data, Conn, Keys>::run() {
  while (true) {
    std::vector<Data>* curr;
    mBatchedQueue->wait_and_pop(curr);
    
    if(mRoundRobinDrIndex < mDataReaders.size()){
      mRoundRobinDrIndex++;
    }
    
    if(mRoundRobinDrIndex >= mDataReaders.size()){
      mRoundRobinDrIndex = 0;
    }
    
    mDataReaders[mRoundRobinDrIndex]->processBatch(++mCurrIndex, curr);
  }
}

template<typename Data, typename Conn, typename Keys>
void NdbDataReaders<Data, Conn, Keys>::processBatch(std::vector<Data>* data_batch) {
  mBatchedQueue->push(data_batch);
}

template<typename Data, typename Conn, typename Keys>
void NdbDataReaders<Data, Conn, Keys>::writeOutput(Bulk<Keys> out) {
  mWaitingOutQueue->push(out);
  processWaiting();
}

template<typename Data, typename Conn, typename Keys>
void NdbDataReaders<Data, Conn, Keys>::processWaiting() {
  while (!mWaitingOutQueue->empty()) {
    boost::optional<Bulk<Keys> > out_ptr = mWaitingOutQueue->pop();
    if (out_ptr) {
      Bulk<Keys> out = out_ptr.get();
      if (out.mProcessingIndex == mLastSent + 1) {
        LOG_INFO("publish enriched events with index [" << out.mProcessingIndex << "] to Elastic");
        timedRestBatcher->addData(out);
        mLastSent++;
      } else {
        mWaitingOutQueue->push(out);
        break;
      }
    } else {
      break;
    }
  }
}

template<typename Data, typename Conn, typename Keys>
NdbDataReaders<Data, Conn, Keys>::~NdbDataReaders() {
  delete mBatchedQueue;
}

#endif /* NDBDATAREADERS_H */

