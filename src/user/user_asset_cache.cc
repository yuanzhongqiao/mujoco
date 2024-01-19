// Copyright 2024 DeepMind Technologies Limited
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

#include "user/user_asset_cache.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// adds a block of data for the asset and returns number of bytes stored
template<typename T>
std::size_t mjCAsset::Add(const std::string& name, const std::vector<T>& v) {
  auto [it, inserted] = blocks_.insert({name, mjCAssetData()});
  if (!inserted) {
    return 0;
  }

  std::size_t n = v.size() * sizeof(T);
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(v.data());
  mjCAssetData& block = it->second;

  block.bytes = std::make_shared<uint8_t[]>(n);
  std::copy(ptr, ptr + n, block.bytes.get());
  block.nbytes = n;
  nbytes_ += n;
  return n;
}

template std::size_t mjCAsset::Add(const std::string& name,
                                   const std::vector<int>& v);
template std::size_t mjCAsset::Add(const std::string& name,
                                   const std::vector<float>& v);
template std::size_t mjCAsset::Add(const std::string& name,
                                   const std::vector<double>& v);



// fetches a block of data, sets n to size of data
template<typename T>
const T* mjCAsset::Get(const std::string& name, std::size_t* n) const {
  auto it = blocks_.find(name);
  if (it == blocks_.end()) {
    *n = 0;
    return nullptr;
  }

  const mjCAssetData& data = it->second;

  // TODO(kylebayes): This is probably caused by a user bug and needs an
  // assertion for debugging purposes
  if (data.nbytes % sizeof(T)) {
    *n = 0;
    return nullptr;
  }

  *n = data.nbytes / sizeof(T);
  return reinterpret_cast<T*>(data.bytes.get());
}

template
const int* mjCAsset::Get(const std::string& name, std::size_t* n) const;
template
const float* mjCAsset::Get(const std::string& name, std::size_t* n) const;
template
const double* mjCAsset::Get(const std::string& name, std::size_t* n) const;



// replaces blocks data in asset
void mjCAsset::ReplaceBlocks(
    const std::unordered_map<std::string, mjCAssetData>& blocks,
    std::size_t nbytes) {
  blocks_ = blocks;
  nbytes_ = nbytes;
}



// makes a copy for user (strip unnecessary items)
mjCAsset mjCAsset::Copy(const mjCAsset& other) {
  mjCAsset asset;
  asset.id_ = other.Id();
  asset.timestamp_ = other.Timestamp();
  asset.blocks_ = other.blocks_;
  asset.nbytes_ = other.nbytes_;
  return asset;
}



// sets the total maximum size of the cache in bytes
// low-priority cached assets will be dropped to make the new memory
// requirement
void mjCCache::SetMaxSize(std::size_t size) {
  std::lock_guard<std::mutex> lock(mutex_);
  max_size_ = size;
  Trim();
}



// returns the corresponding timestamp, if the given asset is stored in the cache
const std::string* mjCCache::HasAsset(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = lookup_.find(id);
  if (it == lookup_.end()) {
    return nullptr;
  }

  return &(it->second.Timestamp());
}



// inserts an asset into the cache, if asset is already in the cache, its data
// is updated only if the timestamps disagree
bool mjCCache::Insert(const mjCAsset& asset) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string& id = asset.Id();
  if (asset.References().size() != 1) {
    return false;
  }
  const std::string& filename = *(asset.References().begin());
  auto [it, inserted] = lookup_.insert({id, asset});

  if (!inserted) {
    mjCAsset* asset_ptr = &(it->second);
    if (size_ - asset_ptr->BytesCount() + asset.BytesCount() > max_size_) {
      return false;
    }
    models_[filename].insert(asset_ptr);  // add it for the model
    asset_ptr->AddReference(filename);
    if (it->second.Timestamp() == asset.Timestamp()) {
      return true;
    }
    asset_ptr->SetTimestamp(asset.Timestamp());
    size_ = size_ - asset_ptr->BytesCount() + asset.BytesCount();
    asset_ptr->ReplaceBlocks(asset.Blocks(), asset.BytesCount());
    return true;
  } else if (size_ + asset.BytesCount() > max_size_) {
    return false;
  }

  // new asset
  mjCAsset* asset_ptr = &(it->second);
  asset_ptr->SetInsertNum(insert_num_++);
  entries_.insert(asset_ptr);
  models_[filename].insert(asset_ptr);
  size_ += asset.BytesCount();
  return true;
}



bool mjCCache::Insert(mjCAsset&& asset) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string& id = asset.Id();
  if (asset.References().size() != 1) {
    return false;
  }
  const std::string& filename = *(asset.References().begin());
  std::size_t nbytes = asset.BytesCount();
  auto [it, inserted] = lookup_.try_emplace(id, std::move(asset));

  if (!inserted) {
    mjCAsset* asset_ptr = &(it->second);
    if (size_ - asset_ptr->BytesCount() + nbytes > max_size_) {
      return false;
    }
    models_[filename].insert(asset_ptr);  // add it for the model
    asset_ptr->AddReference(std::move(filename));
    if (it->second.Timestamp() == asset.Timestamp()) {
      return true;
    }
    // move data and timestamp over
    asset_ptr->SetTimestamp(std::move(asset.timestamp_));
    size_ = size_ - asset_ptr->BytesCount() + nbytes;
    asset_ptr->ReplaceBlocks(std::move(asset.blocks_), asset.nbytes_);
    return true;
  } else if (size_ + nbytes > max_size_) {
    return false;
  }

  // new asset
  mjCAsset* asset_ptr = &(it->second);
  asset_ptr->SetInsertNum(insert_num_++);
  entries_.insert(asset_ptr);
  models_[filename].insert(asset_ptr);
  size_ += nbytes;
  return true;
}



// returns the asset with the given id, if it exists in the cache
std::optional<mjCAsset> mjCCache::Get(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = lookup_.find(id);
  if (it == lookup_.end()) {
    return std::nullopt;
  }

  mjCAsset* asset = &(it->second);
  asset->IncrementAccess();

  // update priority queue
  entries_.erase(asset);
  entries_.insert(asset);
  return asset->Copy(*asset);
}



// removes model from the cache along with assets referencing only this model
void mjCCache::RemoveModel(const std::string& filename) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (mjCAsset* asset : models_[filename]) {
    asset->RemoveReference(filename);
    if (!asset->HasReferences()) {
      Delete(asset, filename);
    }
  }
  models_.erase(filename);
}



// Wipes out all internal data for the given model
void mjCCache::Reset(const std::string& filename) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto asset : models_[filename]) {
    Delete(asset, filename);
  }
  models_.erase(filename);
}



// Wipes out all internal data
void mjCCache::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  lookup_.clear();
  models_.clear();
  size_ = 0;
  insert_num_ = 0;
}



std::size_t mjCCache::MaxSize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return max_size_;
}



std::size_t mjCCache::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}



// Deletes a single asset
void mjCCache::DeleteAsset(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = lookup_.find(id);
  if (it != lookup_.end()) {
     Delete(&(it->second));
  }
}



// Deletes a single asset (internal)
void mjCCache::Delete(mjCAsset* asset) {
  size_ -= asset->BytesCount();
  entries_.erase(asset);
  for (auto& reference : asset->References()) {
    models_[reference].erase(asset);
  }
  lookup_.erase(asset->Id());
}



// Deletes a single asset (internal)
void mjCCache::Delete(mjCAsset* asset, const std::string& skip) {
  size_ -= asset->BytesCount();
  entries_.erase(asset);

  for (auto& reference : asset->References()) {
    if (reference != skip) {
      models_[reference].erase(asset);
     }
  }
  lookup_.erase(asset->Id());
}



// trims out data to meet memory requirements
void mjCCache::Trim() {
  while (size_ > max_size_) {
    Delete(*entries_.begin());
  }
}
