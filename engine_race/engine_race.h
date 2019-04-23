// Copyright [2018] Alibaba Cloud All rights reserved
#ifndef ENGINE_RACE_ENGINE_RACE_H_
#define ENGINE_RACE_ENGINE_RACE_H_
#include <string>
#include <vector>
#include <shared_mutex>
#include <iostream>
#include <deque>
#include <cstdio>
#include <experimental/filesystem>
#include <optional>
#include <condition_variable>
#include <thread>
#include <chrono>
#include "include/engine.h"

#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/containers/map.hpp>
#include <boost/interprocess/allocators/allocator.hpp>

namespace bip = boost::interprocess;
namespace fs = std::experimental::filesystem;
using namespace std::literals;

namespace polar_race {
  const size_t MAX_KEY_LEN = 2048;
  const size_t MAX_VAL_LEN = 5120000;

  const auto JOURNAL_FILE = "JOURNAL";
  const size_t JOURNAL_SIZE = 128;
  const size_t JOURNAL_BACKOFF = 16;

  const auto INDEX_FILE = "INDEX";

  const auto STORE_DIRECTORY = "STORE";
  const auto STORE_MAX_FILESIZE = 10000000; // 10M for now

  const auto WRITER_WAIT_TIMEOUT = 1ms;
  const auto SYNC_WAIT_TIMEOUT = 100us;

  const auto GROW_THRESHOLD = 65536 * 16;
  const auto GROW_CHUNK = 65536 * 256;
  const auto INDEX_INITIAL_CHUNK = 4;

  struct IndexKey {
    size_t len;
    char key[MAX_KEY_LEN];

    IndexKey(const PolarString &ps) {
      len = ps.size();
      memcpy(key, ps.data(), len);
    }

    IndexKey(const std::string &s) {
      len = s.size();
      memcpy(key, s.data(), len);
    }

    bool equals(const PolarString &ano) {
      if(ano.size() != len) return false;
      for(int i = 0; i<len; ++i)
        if(ano.data()[i] != key[i]) return false;
      return true;
    }

    bool operator<(const IndexKey &ano) const {
      for(int i = 0; i<len && i<ano.len; ++i) {
        if(key[i] < ano.key[i]) return true;
        else if(key[i] > ano.key[i]) return false;
      }

      return len < ano.len;
    }
  };

  struct IndexValue {
    size_t file;
    size_t offset;
    size_t len;
  };

  struct JournalEntry {
    size_t ident;
    std::pair<IndexKey, IndexValue> pair;
  };

  class Journal {
    public:
      explicit Journal(const std::string& path, size_t ms) : max_size(ms), ent_counter(0), ent_ident(0) {
        fd = std::fopen(path.c_str(), "a+");
        std::freopen(path.c_str(), "r+", fd);
        restore();
      }

      ~Journal() {
        fclose(fd);
      }
      bool restore();
      bool push(const std::pair<IndexKey, IndexValue> &pair);
      std::optional<IndexValue> fetch(const PolarString &key);
      std::deque<std::pair<IndexKey, IndexValue>>* wait_data(std::unique_lock<std::shared_mutex> &lock);
      std::unique_lock<std::shared_mutex> lock();
    private:
      std::deque<std::pair<IndexKey, IndexValue>> queue;
      std::shared_mutex mut;
      FILE* fd;
      int ent_counter;
      int ent_ident;
      size_t max_size;

      std::condition_variable_any notify_sync;
      std::condition_variable_any notify_writers;
  };

  typedef bip::managed_mapped_file::segment_manager seg_manager;
  typedef bip::allocator<void, seg_manager> void_alloc;

  typedef std::pair<const IndexKey, IndexValue> index_map_type;
  typedef bip::allocator<index_map_type, seg_manager> index_map_type_alloc;
  typedef bip::map<IndexKey, IndexValue, std::less<IndexKey>, index_map_type_alloc> index_map;

  class Index {
    public:
      explicit Index(const std::string& path) : file_path(path) {
        std::cout<<"Initializing index..."<<std::endl;
        reload_file();
        std::cout<<"Index initialized."<<std::endl;
      }

      ~Index() {
        std::cout<<"Dropping index obj..."<<std::endl;
        file->flush();
        delete file;
        std::cout<<"Index obj dropped."<<std::endl;
      }

      void lossy_put(const IndexKey &key, const IndexValue &val);
      void persist();
      void check_free_space();
      std::optional<IndexValue> get(const IndexKey &key);
    private:
      std::string file_path;
      bip::managed_mapped_file *file;
      index_map *map;
      void reload_file();
  };

  class Store {
    public:
      explicit Store(const std::string& path) : basedir(path) {
        fs::create_directory(path);
        for(auto &file : fs::directory_iterator(path)) {
          auto fn = file.path().filename();
          size_t integer = std::stoi(fn);
          std::cout<<"File: "<<integer<<std::endl;
          if(integer >= file_counter) {
            file_counter = integer;
            offset = fs::file_size(file.path());

            std::cout<<"Offset: "<<offset<<std::endl;
          }
        }

        if(file_counter == -1) file_counter = 0;
      }

      template<typename C>
      std::vector<std::pair<IndexKey, IndexValue>> append(const C &vals);
      std::pair<IndexKey, IndexValue> append(std::pair<PolarString, PolarString> val);

      std::string fetch(const IndexValue &loc);
    private:
      std::string basedir;
      size_t file_counter = 0;
      size_t offset = 0;
      FILE* get_fd(size_t file, size_t offset);
      std::shared_mutex fs_mut;
  };

  class EngineRace : public Engine  {
    public:
      static RetCode Open(const std::string& name, Engine** eptr);

      explicit EngineRace(const std::string& dir) : journal(dir+"/"+JOURNAL_FILE, JOURNAL_SIZE), index(dir+"/"+INDEX_FILE), store(dir+"/"+STORE_DIRECTORY) {
        sync_worker = std::thread([this]() {
          auto lock = this->journal.lock();

          while(true) {
            auto data = journal.wait_data(lock);
            this->clear_queue(data);
            if(this->halt) {
              break;
            }
          }
        });
      }

      ~EngineRace() {
        halt = true;
        sync_worker.join();
      }

      RetCode Write(const PolarString& key,
          const PolarString& value) override;

      RetCode Read(const PolarString& key,
          std::string* value) override;

      /*
       * NOTICE: Implement 'Range' in quarter-final,
       *         you can skip it in preliminary.
       */
      RetCode Range(const PolarString& lower,
          const PolarString& upper,
          Visitor &visitor) override;

    private: 
      Journal journal;
      Index index;
      Store store;

      std::shared_mutex read_lock;

      template<typename C>
      void clear_queue(C *queue);

      std::thread sync_worker;
      bool halt = false;
  };
}  // namespace polar_race

#endif  // ENGINE_RACE_ENGINE_RACE_H_
