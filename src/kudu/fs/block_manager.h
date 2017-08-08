// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kudu/fs/block_id.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/status.h"

DECLARE_bool(block_coalesce_close);

namespace kudu {

class Env;
class MemTracker;
class MetricEntity;
class Slice;

namespace fs {

class BlockManager;
class DataDirManager;
class FsErrorManager;
struct FsReport;

// The smallest unit of Kudu data that is backed by the local filesystem.
//
// The block interface reflects Kudu on-disk storage design principles:
// - Blocks are append only.
// - Blocks are immutable once written.
// - Blocks opened for reading are thread-safe and may be used by multiple
//   concurrent readers.
// - Blocks opened for writing are not thread-safe.
class Block {
 public:
  virtual ~Block() {}

  // Returns the identifier for this block.
  virtual const BlockId& id() const = 0;
};

// A block that has been opened for writing. There may only be a single
// writing thread, and data may only be appended to the block.
//
// Close() is an expensive operation, as it must flush both dirty block data
// and metadata to disk. The block manager API provides two ways to improve
// Close() performance:
// 1. FlushDataAsync() before Close(). If there's enough work to be done
//    between the two calls, there will be less outstanding I/O to wait for
//    during Close().
// 2. CloseBlocks() on a group of blocks. This at least ensures that, when
//    waiting on outstanding I/O, the waiting is done in parallel.
//
// NOTE: if a WritableBlock is not explicitly Close()ed, it will be aborted
// (i.e. deleted).
class WritableBlock : public Block {
 public:
  enum State {
    // There is no dirty data in the block.
    CLEAN,

    // There is some dirty data in the block.
    DIRTY,

    // There is an outstanding flush operation asynchronously flushing
    // dirty block data to disk.
    FLUSHING,

    // The block is closed. No more operations can be performed on it.
    CLOSED
  };

  // Destroy the WritableBlock. If it was not explicitly closed using Close(),
  // this will Abort() the block.
  virtual ~WritableBlock() {}

  // Destroys the in-memory representation of the block and synchronizes
  // dirty block data and metadata with the disk. On success, guarantees
  // that the entire block is durable.
  virtual Status Close() = 0;

  // Like Close() but does not synchronize dirty data or metadata to disk.
  // Meaning, after a successful Abort(), the block no longer exists.
  virtual Status Abort() = 0;

  // Get a pointer back to this block's manager.
  virtual BlockManager* block_manager() const = 0;

  // Appends the chunk of data referenced by 'data' to the block.
  //
  // Does not guarantee durability of 'data'; Close() must be called for all
  // outstanding data to reach the disk.
  virtual Status Append(const Slice& data) = 0;

  // Appends multiple chunks of data referenced by 'data' to the block.
  //
  // Does not guarantee durability of 'data'; Close() must be called for all
  // outstanding data to reach the disk.
  virtual Status AppendV(const std::vector<Slice>& data) = 0;

  // Begins an asynchronous flush of dirty block data to disk.
  //
  // This is purely a performance optimization for Close(); if there is
  // other work to be done between the final Append() and the future
  // Close(), FlushDataAsync() will reduce the amount of time spent waiting
  // for outstanding I/O to complete in Close(). This is analogous to
  // readahead or prefetching.
  //
  // Data may not be written to the block after FlushDataAsync() is called.
  virtual Status FlushDataAsync() = 0;

  // Returns the number of bytes successfully appended via Append().
  virtual size_t BytesAppended() const = 0;

  virtual State state() const = 0;
};

// A block that has been opened for reading. Multiple in-memory blocks may
// be constructed for the same logical block, and the same in-memory block
// may be shared amongst threads for concurrent reading.
class ReadableBlock : public Block {
 public:
  virtual ~ReadableBlock() {}

  // Destroys the in-memory representation of the block.
  virtual Status Close() = 0;

  // Returns the on-disk size of a written block.
  virtual Status Size(uint64_t* sz) const = 0;

  // Reads exactly 'result.size' bytes beginning from 'offset' in the block,
  // returning an error if fewer bytes exist.
  // Sets "result" to the data that was read.
  // If an error was encountered, returns a non-OK status.
  virtual Status Read(uint64_t offset, Slice* result) const = 0;

  // Reads exactly the "results" aggregate bytes, based on each Slice's "size",
  // beginning from 'offset' in the block, returning an error if fewer bytes exist.
  // Sets each "result" to the data that was read.
  // If an error was encountered, returns a non-OK status.
  virtual Status ReadV(uint64_t offset, std::vector<Slice>* results) const = 0;

  // Returns the memory usage of this object including the object itself.
  virtual size_t memory_footprint() const = 0;
};

// Provides options and hints for block placement. This is used for identifying
// the correct DataDirGroups to place blocks. In the future this may also be
// used to specify directories based on block type (e.g. to prefer bloom block
// placement into SSD-backed directories).
struct CreateBlockOptions {
  const std::string tablet_id;
};

// Block manager creation options.
struct BlockManagerOptions {
  BlockManagerOptions();
  ~BlockManagerOptions();

  // The entity under which all metrics should be grouped. If NULL, metrics
  // will not be produced.
  //
  // Defaults to NULL.
  scoped_refptr<MetricEntity> metric_entity;

  // The memory tracker under which all new memory trackers will be parented.
  // If NULL, new memory trackers will be parented to the root tracker.
  std::shared_ptr<MemTracker> parent_mem_tracker;

  // The paths where data blocks will be stored. Cannot be empty.
  std::vector<std::string> root_paths;

  // Whether the block manager should only allow reading. Defaults to false.
  bool read_only;
};

// Utilities for Kudu block lifecycle management. All methods are
// thread-safe.
class BlockManager {
 public:
  virtual ~BlockManager() {}

  // Creates a new on-disk representation for this block manager. Must be
  // followed up with a call to Open() to use the block manager.
  //
  // Returns an error if one already exists or cannot be created.
  virtual Status Create() = 0;

  // Opens an existing on-disk representation of this block manager and
  // checks it for inconsistencies. If found, and if the block manager was not
  // constructed in read-only mode, an attempt will be made to repair them.
  //
  // If 'report' is not nullptr, it will be populated with the results of the
  // check (and repair, if applicable); otherwise, the results of the check
  // will be logged and the presence of fatal inconsistencies will manifest as
  // a returned error.
  //
  // Returns an error if an on-disk representation does not exist or cannot be
  // opened.
  virtual Status Open(FsReport* report) = 0;

  // Creates a new block using the provided options and opens it for
  // writing. The block's ID will be generated.
  //
  // Does not guarantee the durability of the block; it must be closed to
  // ensure that it reaches disk.
  //
  // Does not modify 'block' on error.
  virtual Status CreateBlock(const CreateBlockOptions& opts,
                             std::unique_ptr<WritableBlock>* block) = 0;

  // Opens an existing block for reading.
  //
  // While it is safe to delete a block that has already been opened, it is
  // not safe to do so concurrently with the OpenBlock() call itself. In some
  // block manager implementations this may result in unusual behavior. For
  // example, OpenBlock() may succeed but subsequent ReadableBlock operations
  // may fail.
  //
  // Does not modify 'block' on error.
  virtual Status OpenBlock(const BlockId& block_id,
                           std::unique_ptr<ReadableBlock>* block) = 0;

  // Deletes an existing block, allowing its space to be reclaimed by the
  // filesystem. The change is immediately made durable.
  //
  // Blocks may be deleted while they are open for reading or writing;
  // the actual deletion will take place after the last open reader or
  // writer is closed.
  virtual Status DeleteBlock(const BlockId& block_id) = 0;

  // Closes (and fully synchronizes) the given blocks. Effectively like
  // Close() for each block but may be optimized for groups of blocks.
  //
  // On success, guarantees that outstanding data is durable.
  virtual Status CloseBlocks(const std::vector<WritableBlock*>& blocks) = 0;

  // Retrieves the IDs of all blocks under management by this block manager.
  // These include ReadableBlocks as well as WritableBlocks.
  //
  // Returned block IDs are not guaranteed to be in any particular order,
  // nor is the order guaranteed to be deterministic. Furthermore, if
  // concurrent operations are ongoing, some of the blocks themselves may not
  // even exist after the call.
  virtual Status GetAllBlockIds(std::vector<BlockId>* block_ids) = 0;

  // Exposes the underlying DataDirManager.
  virtual DataDirManager* dd_manager() = 0;

  // Exposes the FsErrorManager used to handle fs errors.
  virtual FsErrorManager* error_manager() = 0;
};

// Closes a group of blocks.
//
// Blocks must be closed explicitly via CloseBlocks(), otherwise they will
// be deleted in the in the destructor.
class ScopedWritableBlockCloser {
 public:
  ScopedWritableBlockCloser() {}

  ~ScopedWritableBlockCloser() {
    for (WritableBlock* block : blocks_) {
      WARN_NOT_OK(block->Abort(), strings::Substitute(
          "Failed to abort block with id $0", block->id().ToString()));
    }
    STLDeleteElements(&blocks_);
  }

  void AddBlock(std::unique_ptr<WritableBlock> block) {
    blocks_.push_back(block.release());
  }

  Status CloseBlocks() {
    if (blocks_.empty()) {
      return Status::OK();
    }
    ElementDeleter deleter(&blocks_);

    // We assume every block is using the same block manager, so any
    // block's manager will do.
    BlockManager* bm = blocks_[0]->block_manager();
    return bm->CloseBlocks(blocks_);
  }

  const std::vector<WritableBlock*>& blocks() const { return blocks_; }

 private:
  std::vector<WritableBlock*> blocks_;
};

// Compute an upper bound for a file cache embedded within a block manager
// using resource limits obtained from the system.
int64_t GetFileCacheCapacityForBlockManager(Env* env);

} // namespace fs
} // namespace kudu
