// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_edit.h"

#include "db/version_set.h"
#include "util/coding.h"

namespace leveldb {

// Tag numbers for serialized VersionEdit.  These numbers are written to
// disk and should not be changed.
// 用于标记manifest文件中的各种数据类型
enum Tag {
  kComparator           = 1,
  kLogNumber            = 2,
  kNextFileNumber       = 3,
  kLastSequence         = 4,
  kCompactPointer       = 5,
  kDeletedFile          = 6,
  kNewFile              = 7,
  // 8 was used for large value refs
  kPrevLogNumber        = 9
};

void VersionEdit::Clear() {
  comparator_.clear();
  log_number_ = 0;
  prev_log_number_ = 0;
  last_sequence_ = 0;
  next_file_number_ = 0;
  has_comparator_ = false;
  has_log_number_ = false;
  has_prev_log_number_ = false;
  has_next_file_number_ = false;
  has_last_sequence_ = false;
  deleted_files_.clear();
  new_files_.clear();
}

// 将edit持久化:tag+值，tag+值，...
void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) {
    PutVarint32(dst, kComparator);// 将比较器持久化
    PutLengthPrefixedSlice(dst, comparator_);
  }
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);// 日志号的tag
    PutVarint64(dst, log_number_);//日志文件号持久化 
  }
  if (has_prev_log_number_) {
    PutVarint32(dst, kPrevLogNumber); // 上一个日志的tag
    PutVarint64(dst, prev_log_number_);// 上一个日志文件
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);// 下一个日志文件
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);// 最大的序列号
  }

  for (size_t i = 0; i < compact_pointers_.size(); i++) {
    PutVarint32(dst, kCompactPointer);
    PutVarint32(dst, compact_pointers_[i].first);  // level
    PutLengthPrefixedSlice(dst, compact_pointers_[i].second.Encode());
  }
  
  // 删除文件：{level, file number}
  for (DeletedFileSet::const_iterator iter = deleted_files_.begin();
       iter != deleted_files_.end();
       ++iter) {
    PutVarint32(dst, kDeletedFile);// 删除文件类型
    PutVarint32(dst, iter->first);   // level
    PutVarint64(dst, iter->second);  // file number
  }

  // 增加文件:{}
  for (size_t i = 0; i < new_files_.size(); i++) {
    const FileMetaData& f = new_files_[i].second;
    PutVarint32(dst, kNewFile); //增加文件类型
    PutVarint32(dst, new_files_[i].first);  // level
    PutVarint64(dst, f.number); // 文件编号
    PutVarint64(dst, f.file_size); // sst文件大小
    PutLengthPrefixedSlice(dst, f.smallest.Encode()); // 最小值
    PutLengthPrefixedSlice(dst, f.largest.Encode()); // 最大值
  }
}

static bool GetInternalKey(Slice* input, InternalKey* dst) {
  Slice str;
  if (GetLengthPrefixedSlice(input, &str)) {
    dst->DecodeFrom(str);
    return true;
  } else {
    return false;
  }
}

// 解析出level值
static bool GetLevel(Slice* input, int* level) {
  uint32_t v;
  if (GetVarint32(input, &v) &&
      v < config::kNumLevels) {
    *level = v;
    return true;
  } else {
    return false;
  }
}

// 根据字符串，进行edit构建
Status VersionEdit::DecodeFrom(const Slice& src) {
  Clear();
  Slice input = src;
  const char* msg = NULL;
  uint32_t tag;

  // Temporary storage for parsing
  int level;
  uint64_t number;
  FileMetaData f;
  Slice str;
  InternalKey key;
  
  // 遍历数据流，首先判断出tag，再解析出值
  while (msg == NULL && GetVarint32(&input, &tag)) {
    switch (tag) {
      case kComparator:
        // 获得比较器名字
        if (GetLengthPrefixedSlice(&input, &str)) {
          comparator_ = str.ToString();
          has_comparator_ = true;
        } else {
          msg = "comparator name";
        }
        break;

      case kLogNumber:
        if (GetVarint64(&input, &log_number_)) {
          has_log_number_ = true;
        } else {
          msg = "log number";
        }
        break;

      case kPrevLogNumber:
        if (GetVarint64(&input, &prev_log_number_)) {
          has_prev_log_number_ = true;
        } else {
          msg = "previous log number";
        }
        break;

      case kNextFileNumber:
        if (GetVarint64(&input, &next_file_number_)) {
          has_next_file_number_ = true;
        } else {
          msg = "next file number";
        }
        break;

      case kLastSequence:
        if (GetVarint64(&input, &last_sequence_)) {
          has_last_sequence_ = true;
        } else {
          msg = "last sequence number";
        }
        break;

      case kCompactPointer:
        if (GetLevel(&input, &level) &&
            GetInternalKey(&input, &key)) {
          compact_pointers_.push_back(std::make_pair(level, key));
        } else {
          msg = "compaction pointer";
        }
        break;

      case kDeletedFile:
        if (GetLevel(&input, &level) &&
            GetVarint64(&input, &number)) {
          deleted_files_.insert(std::make_pair(level, number));
        } else {
          msg = "deleted file";
        }
        break;

      case kNewFile:
        if (GetLevel(&input, &level) &&
            GetVarint64(&input, &f.number) &&
            GetVarint64(&input, &f.file_size) &&
            GetInternalKey(&input, &f.smallest) &&
            GetInternalKey(&input, &f.largest)) {
          new_files_.push_back(std::make_pair(level, f));
        } else {
          msg = "new-file entry";
        }
        break;

      default:
        msg = "unknown tag";
        break;
    }
  }

  if (msg == NULL && !input.empty()) {
    msg = "invalid tag";
  }

  Status result;
  if (msg != NULL) {
    result = Status::Corruption("VersionEdit", msg);
  }
  return result;
}

// 将edit进行可读化打印
std::string VersionEdit::DebugString() const {
  std::string r;
  r.append("VersionEdit {");
  if (has_comparator_) {
    r.append("\n  Comparator: ");
    r.append(comparator_);
  }
  if (has_log_number_) {
    r.append("\n  LogNumber: ");
    AppendNumberTo(&r, log_number_);
  }
  if (has_prev_log_number_) {
    r.append("\n  PrevLogNumber: ");
    AppendNumberTo(&r, prev_log_number_);
  }
  if (has_next_file_number_) {
    r.append("\n  NextFile: ");
    AppendNumberTo(&r, next_file_number_);
  }
  if (has_last_sequence_) {
    r.append("\n  LastSeq: ");
    AppendNumberTo(&r, last_sequence_);
  }
  for (size_t i = 0; i < compact_pointers_.size(); i++) {
    r.append("\n  CompactPointer: ");
    AppendNumberTo(&r, compact_pointers_[i].first);
    r.append(" ");
    r.append(compact_pointers_[i].second.DebugString());
  }
  for (DeletedFileSet::const_iterator iter = deleted_files_.begin();
       iter != deleted_files_.end();
       ++iter) {
    r.append("\n  DeleteFile: ");
    AppendNumberTo(&r, iter->first);
    r.append(" ");
    AppendNumberTo(&r, iter->second);
  }
  for (size_t i = 0; i < new_files_.size(); i++) {
    const FileMetaData& f = new_files_[i].second;
    r.append("\n  AddFile: ");
    AppendNumberTo(&r, new_files_[i].first);
    r.append(" ");
    AppendNumberTo(&r, f.number);
    r.append(" ");
    AppendNumberTo(&r, f.file_size);
    r.append(" ");
    r.append(f.smallest.DebugString());
    r.append(" .. ");
    r.append(f.largest.DebugString());
  }
  r.append("\n}\n");
  return r;
}

Status VersionEdit::SplitEdit(InternalKey& ikey, 
                    const InternalKeyComparator& icmp,
                    int direct) 
{
  Status s;
  //clear compact_pointer
  compact_pointers_.clear();
  std::vector<uint64_t> EraseFile;
  std::vector<std::pair<int, FileMetaData> > tmpFM;
  
  std::vector<std::pair<int, FileMetaData> >::iterator it = new_files_.begin();
  while (it != new_files_.end()) {
    FileMetaData& f = it->second;
    if (direct == SPLIT_LEFT) {
      if (icmp.Compare(ikey, f.smallest) < 0) {
        it = new_files_.erase(it);
        continue;
      } else if (icmp.Compare(f.largest, ikey) >= 0) {
        f.largest = ikey;
      }
    } else {
      if (icmp.Compare(ikey, f.largest) >= 0) {
        it = new_files_.erase(it);
        continue;
      } else if (icmp.Compare(f.smallest, ikey) < 0) {
        f.smallest = ikey;
      }
    }
    it++;
  }
  
  goto out;
  
  // discard below
  for (int i = 0; i < new_files_.size(); i++) {
    if (direct == SPLIT_LEFT) {
      //int level = new_files_[i].first;
      FileMetaData& f = new_files_[i].second;
      /*const InternalKey& prev_end = v->files_[level][i-1]->largest;
        const InternalKey& this_begin = v->files_[level][i]->smallest;
        if (vset_->icmp_.Compare(prev_end, this_begin) >= 0) {
        int FindFile(const InternalKeyComparator& icmp,
        const std::vector<FileMetaData*>& files,
        const Slice& key) {
        */
      if (icmp.Compare(ikey, f.smallest) < 0) {
        //EraseFile.push_back(&(new_files_[i].second)); 
      } else if (icmp.Compare(f.largest, ikey) >= 0) {
        f.largest = ikey;
        tmpFM.push_back(new_files_[i]);
      } else {
        tmpFM.push_back(new_files_[i]);
      }
    } else { // right half
    
    }
  }
  
  new_files_.clear();
  for (int i = 0; i < tmpFM.size(); i++) {
    new_files_.push_back(tmpFM[i]);
  }

out:
  //printf("sizeof new_files_ %u\n", new_files_.size());
  return s;
}

}  // namespace leveldb
