#include "blazingdb/transport/io/reader_writer.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include "blazingdb/transport/io/fd_reader_writer.h"
#include "rmm/rmm.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stack>
#include <thread>
#include <vector>

#include <cassert>
#include <queue>
#include "blazingdb/transport/ColumnTransport.h"
#include "blazingdb/concurrency/BlazingThread.h"
#include <rmm/device_buffer.hpp>
#include <cudf/utilities/error.hpp>
#include "../engine/src/CodeTimer.h"
using namespace std::chrono_literals;

#include <spdlog/spdlog.h>
using namespace fmt::literals;

namespace blazingdb {
namespace transport {
namespace io {


PinnedBuffer::~PinnedBuffer(){
  this->provider->freeBuffer(this->buffer);
}

// numBuffers should be equal to number of threads
PinnedBufferProvider::PinnedBufferProvider(std::size_t sizeBuffers,
                                           std::size_t numBuffers) {
  for (int bufferIndex = 0; bufferIndex < numBuffers; bufferIndex++) {
    PinnedBufferData *buffer = new PinnedBufferData();
    buffer->size = sizeBuffers;
    this->bufferSize = sizeBuffers;
    cudaError_t err = cudaMallocHost((void **)&buffer->data, sizeBuffers);
    if (err != cudaSuccess) {
      throw std::exception();
    }
    this->buffers.push(buffer);
  }
}

// TODO: consider adding some kind of priority
// based on when the request was made
std::shared_ptr<PinnedBuffer> PinnedBufferProvider::getBuffer() {
  std::unique_lock<std::mutex> lock(inUseMutex);
  if (this->buffers.empty()) {
    // cv.wait(lock, [this] { return !this->buffers.empty(); });
    // if wait fail use:
    this->grow();
  }
  PinnedBufferData *temp = this->buffers.top();
  this->buffers.pop();
  return std::make_shared<PinnedBuffer>(this,temp);
}

void PinnedBufferProvider::grow() {
  PinnedBufferData *buffer = new PinnedBufferData();
  buffer->size = this->bufferSize;
  cudaError_t err = cudaMallocHost((void **)&buffer->data, this->bufferSize);
  if (err != cudaSuccess) {
    throw std::exception();
  }
  this->buffers.push(buffer);
}

void PinnedBufferProvider::freeBuffer(PinnedBufferData *buffer) {
  std::unique_lock<std::mutex> lock(inUseMutex);
  this->buffers.push(buffer);
  cv.notify_one();
}

void PinnedBufferProvider::freeAll() {
  std::unique_lock<std::mutex> lock(inUseMutex);
  while (false == this->buffers.empty()) {
    PinnedBufferData *buffer = this->buffers.top();
    cudaFree(buffer->data);
    delete buffer;
    this->buffers.pop();
  }
}

std::size_t PinnedBufferProvider::sizeBuffers() { return this->bufferSize; }

static std::shared_ptr<PinnedBufferProvider> global_instance{};

void setPinnedBufferProvider(std::size_t sizeBuffers, std::size_t numBuffers) {
  global_instance =
      std::make_shared<PinnedBufferProvider>(sizeBuffers, numBuffers);
}

PinnedBufferProvider &getPinnedBufferProvider() { return *global_instance; }

void writeBuffersFromGPUTCP(std::vector<ColumnTransport> &column_transport,
                            std::vector<std::size_t> bufferSizes,
                            std::vector<const char *> buffers, void *fileDescriptor,
                            int gpuNum) {

  if (bufferSizes.size() == 0) {
    return;
  }
  struct queue_item {
    std::size_t bufferIndex{};
    std::size_t chunkIndex{};
    std::shared_ptr<PinnedBuffer> chunk{nullptr};
    std::size_t chunk_size{};

    bool operator<(const queue_item &item) const {
      if (bufferIndex == item.bufferIndex) {
        return chunkIndex > item.chunkIndex;
      } else {
        return bufferIndex > item.bufferIndex;
      }
    }

    bool operator==(const queue_item &item) const {
      return ((bufferIndex == item.bufferIndex) &&
              (chunkIndex == item.chunkIndex));
    }
  };
  std::priority_queue<queue_item> writePairs;
  std::condition_variable cv;
  std::mutex writeMutex;
  std::vector<char *> tempReadAllocations(bufferSizes.size());
  std::vector<BlazingThread> copyThreads(bufferSizes.size());
  std::size_t amountWrittenTotalTotal = 0;

  std::vector<queue_item> writeOrder;
  for (size_t bufferIndex = 0; bufferIndex < bufferSizes.size();
       bufferIndex++) {
    std::size_t amountWrittenTotal = 0;
    size_t chunkIndex = 0;
    do {
      writeOrder.push_back(
          {.bufferIndex = bufferIndex, .chunkIndex = chunkIndex});
      amountWrittenTotal += getPinnedBufferProvider().sizeBuffers();
      chunkIndex++;
    } while (amountWrittenTotal < bufferSizes[bufferIndex]);
  }

  // buffer is from gpu or is from cpu
  for (size_t bufferIndex = 0; bufferIndex < bufferSizes.size();
       bufferIndex++) {
    copyThreads[bufferIndex] = BlazingThread(
        [bufferIndex, &cv, &amountWrittenTotalTotal, &writeMutex, &buffers,
         &writePairs, &writeOrder, &bufferSizes, &tempReadAllocations,
         fileDescriptor, gpuNum]() {
          cudaSetDevice(gpuNum);
          cudaStream_t stream;
          CUDA_TRY(cudaStreamCreate( &stream) );
          std::size_t amountWrittenTotal = 0;
          size_t chunkIndex = 0;
          do {
            std::shared_ptr<PinnedBuffer> buffer = getPinnedBufferProvider().getBuffer();
            std::size_t amountToWrite;
            if ((bufferSizes[bufferIndex] - amountWrittenTotal) > buffer->size())
              amountToWrite = buffer->size();
            else
              amountToWrite = bufferSizes[bufferIndex] - amountWrittenTotal;

            cudaSetDevice(gpuNum);
            cudaMemcpyAsync(buffer->data(),
                            buffers[bufferIndex] + amountWrittenTotal,
                            amountToWrite, cudaMemcpyDeviceToHost, stream);
            cudaStreamSynchronize(stream);

            {
              std::unique_lock<std::mutex> lock(writeMutex);
              writePairs.push(queue_item{.bufferIndex = bufferIndex,
                                         .chunkIndex = chunkIndex,
                                         .chunk = buffer,
                                         .chunk_size = amountToWrite});
              chunkIndex++;
              amountWrittenTotal += amountToWrite;
              cv.notify_one();
            }
          } while (amountWrittenTotal < bufferSizes[bufferIndex]);
          cudaStreamDestroy(stream);
          {
            std::lock_guard<std::mutex> lock(writeMutex);
            amountWrittenTotalTotal += amountWrittenTotal;
          }
        });
  }

  BlazingThread writeThread =
      BlazingThread([fileDescriptor, &writePairs, &bufferSizes, writeOrder,
                   &writeMutex, &cv] {
        std::shared_ptr<PinnedBuffer> buffer = nullptr;
        std::size_t amountToWrite;
        queue_item item;
        std::size_t writeIndex = 0;
        bool started = false;
        do {
          {
            CodeTimer blazing_timer;
            std::unique_lock<std::mutex> lock(writeMutex);
            while(!cv.wait_for(lock, 60000ms, [&writePairs, &writeOrder, writeIndex, &blazing_timer] {
                bool wrote = !writePairs.empty() &&
                      writeOrder[writeIndex] == writePairs.top();

                if (!wrote && blazing_timer.elapsed_time() > 59000){
                  auto logger = spdlog::get("batch_logger");
                  logger->warn("|||{info}|{duration}||||",
                              "info"_a="writeBuffersFromGPUTCP timed out",
                              "duration"_a=blazing_timer.elapsed_time());
                }
                return wrote;
              })){}

            item = writePairs.top();
            amountToWrite = item.chunk_size;
            buffer = item.chunk;
            started = false;
            writePairs.pop();
          }

          if (buffer != nullptr) {
            std::lock_guard<std::mutex> lock(writeMutex);
            blazingdb::transport::io::writeToSocket(
                fileDescriptor, (char *)buffer->data(), amountToWrite);
            writeIndex++;

          }
        } while (writeIndex < writeOrder.size());
      });

  for (std::size_t threadIndex = 0; threadIndex < copyThreads.size();
       threadIndex++) {
    copyThreads[threadIndex].join();
  }
  {
    std::unique_lock<std::mutex> lock(writeMutex);
    writePairs.push({.bufferIndex = INT_MAX,
                     .chunkIndex = INT_MAX,
                     .chunk = nullptr,
                     .chunk_size = amountWrittenTotalTotal});
    cv.notify_one();
  }
  writeThread.join();

}

void readBuffersIntoGPUTCP(std::vector<std::size_t> bufferSizes,
                                          void *fileDescriptor, int gpuNum, std::vector<rmm::device_buffer> &tempReadAllocations)
{
  cudaStream_t stream;
  CUDA_TRY(cudaStreamCreate( &stream) );
  for (int bufferIndex = 0; bufferIndex < bufferSizes.size(); bufferIndex++) {
    cudaSetDevice(gpuNum);
    tempReadAllocations.emplace_back(rmm::device_buffer(bufferSizes[bufferIndex]));
  }
  for (int bufferIndex = 0; bufferIndex < bufferSizes.size(); bufferIndex++) {
    std::vector<BlazingThread> copyThreads;
    std::size_t amountReadTotal = 0;
    do {
      std::shared_ptr<PinnedBuffer> buffer = getPinnedBufferProvider().getBuffer();
      std::size_t amountToRead =
          (bufferSizes[bufferIndex] - amountReadTotal) > buffer->size()
              ? buffer->size()
              : bufferSizes[bufferIndex] - amountReadTotal;

      blazingdb::transport::io::readFromSocket(fileDescriptor, (char *)buffer->data(), amountToRead);


      copyThreads.push_back(BlazingThread(
          [&tempReadAllocations, &bufferSizes, bufferIndex,
           buffer, amountToRead, amountReadTotal, gpuNum, stream]() {
            cudaSetDevice(gpuNum);
            cudaMemcpyAsync(tempReadAllocations[bufferIndex].data() + amountReadTotal,
                            buffer->data(), amountToRead, cudaMemcpyHostToDevice,
                            stream);
            cudaStreamSynchronize(stream); //have to synchronize here becuase the buffer will go out of scope if we dont
          }));
      amountReadTotal += amountToRead;

    } while (amountReadTotal < bufferSizes[bufferIndex]);

    for (std::size_t threadIndex = 0; threadIndex < copyThreads.size(); threadIndex++) {
      copyThreads[threadIndex].join();
    }
    cudaStreamDestroy(stream);
  }
  // return tempReadAllocations;
}

void readBuffersIntoCPUTCP(std::vector<std::size_t> bufferSizes,
                                          void *fileDescriptor, int gpuNum, std::vector<Buffer> & tempReadAllocations)
{
  cudaStream_t stream;
  CUDA_TRY(cudaStreamCreate( &stream) );
  for (int bufferIndex = 0; bufferIndex < bufferSizes.size(); bufferIndex++) {
    cudaSetDevice(gpuNum);
    tempReadAllocations.emplace_back(Buffer(bufferSizes[bufferIndex], '0'));
  }
  for (int bufferIndex = 0; bufferIndex < bufferSizes.size(); bufferIndex++) {
    std::vector<BlazingThread> copyThreads;
    std::size_t amountReadTotal = 0;
    do {
      std::shared_ptr<PinnedBuffer> buffer = getPinnedBufferProvider().getBuffer();
      std::size_t amountToRead =
          (bufferSizes[bufferIndex] - amountReadTotal) > buffer->size()
              ? buffer->size()
              : bufferSizes[bufferIndex] - amountReadTotal;
      blazingdb::transport::io::readFromSocket(fileDescriptor, (char *)buffer->data(), amountToRead);


      copyThreads.push_back(BlazingThread(
          [&tempReadAllocations, &bufferSizes, bufferIndex,
           buffer, amountToRead, amountReadTotal, gpuNum,stream]() {
            cudaSetDevice(gpuNum);
            cudaMemcpyAsync((void *)tempReadAllocations[bufferIndex].data() + amountReadTotal,
                            buffer->data(), amountToRead, cudaMemcpyHostToHost, // use cudaMemcpyHostToHost for lazy loading into gpu memory
                            stream);
            cudaStreamSynchronize(stream);

          }));
      amountReadTotal += amountToRead;

    } while (amountReadTotal < bufferSizes[bufferIndex]);
    for (std::size_t threadIndex = 0; threadIndex < copyThreads.size(); threadIndex++) {
      copyThreads[threadIndex].join();
    }
  }

  cudaStreamDestroy(stream);
}


}  // namespace io
}  // namespace transport
}  // namespace blazingdb
