// Copyright [2018] Alibaba Cloud All rights reserved
#include "engine_race.h"
#include <mutex>
#include <shared_mutex>
#include <iostream>

namespace polar_race {
  bool Journal::restore() {
    std::unique_lock<std::shared_mutex> lock(mut);
    std::fseek(fd, 0, SEEK_SET);
    auto entries_mem = new char[sizeof(JournalEntry) * max_size];
    auto entries = (JournalEntry*) entries_mem;

    size_t count = std::fread(entries, sizeof(JournalEntry), max_size, fd);

    if(count < max_size) {
      ent_counter = count;
      ent_ident = count;
    } else {
      ent_counter = 0;
      ent_ident = 0;
      for(size_t i = 1; i<count; ++i) {
        if(entries[i].ident != (entries[i-1].ident + 1) % (max_size + 1)) {
          ent_counter = i;
          ent_ident = (entries[i-1].ident + 1) % (max_size + 1);
          break;
        }
      }
    }

    if(count < max_size) {
      for(int i = 0; i<count; ++i) {
        const auto &ent = entries[i];
        // std::cout<<"Restored: "<<ent.ident<<" "<<PolarString(ent.key, ent.keylen).ToString()<<std::endl;
        queue.push_back(ent.pair);
      }
    } else {
      size_t iter = ent_counter;
      while(true) {
        const auto &ent = entries[iter];
        // std::cout<<"Restored: "<<ent.ident<<" "<<PolarString(ent.key, ent.keylen).ToString()<<std::endl;
        queue.push_back(ent.pair);
        ++iter;
        if(iter == max_size) iter = 0;
        if(iter == ent_counter) break;
      }
    }

    // std::cout<<"Seeking..."<<std::endl;
    if(count < max_size)
      std::fseek(fd, 0, SEEK_END);
    else
      std::fseek(fd, sizeof(JournalEntry) * ent_counter, SEEK_SET);
    // std::cout<<"Seeked."<<std::endl;

    // Don't drop buf, as it needs to be persisted in the queue
    return true;
  }

  bool Journal::push(const std::pair<IndexKey, IndexValue> &pair) {
    // Journal is rarely full, so we are checking for that inside
    std::unique_lock<std::shared_mutex> lock(mut);
    while(queue.size() == max_size) {
      notify_sync.notify_one();
      notify_writers.wait_for(lock, WRITER_WAIT_TIMEOUT);
    }

    queue.push_back(pair);

    JournalEntry ent {
      .ident = (size_t) ent_ident,
      .pair = pair,
    };

    if(++ent_ident == max_size + 1)
      ent_ident = 0;
    
    // Write
    // We don't care about endian, because we are running on the same computer
    std::fwrite(&ent, sizeof(JournalEntry), 1, fd);
    std::fflush(fd);
    if(++ent_counter == max_size) {
      ent_counter = 0;
      std::fseek(fd, 0, SEEK_SET);
    }

    return true;
  }

  std::deque<std::pair<IndexKey, IndexValue>>* Journal::wait_data(std::unique_lock<std::shared_mutex> &lock) {
    notify_writers.notify_all();
    notify_sync.wait_for(lock, SYNC_WAIT_TIMEOUT);
    return &queue;
  }

  std::unique_lock<std::shared_mutex> Journal::lock() {
    return std::unique_lock(mut);
  }

  std::optional<IndexValue> Journal::fetch(const PolarString &key) {
    std::shared_lock<std::shared_mutex> lock(mut);
    for(auto it = queue.rbegin(); it != queue.rend(); ++it)
      if(it->first.equals(key))
        return it->second;

    return {};
  }

  std::optional<IndexValue> Index::lossy_put(const IndexKey &key, const IndexValue &val) {
    auto it = map->find(key);

    if(it != map->end()) {
      auto original = it->second;
      it->second = val;
      return { original };
    } else {
      map->insert(std::make_pair(key, val));
      return {};
    }
  }

  void Index::persist() {
    file->flush();
  }

  void Index::check_free_space() {
    if(file->get_segment_manager()->get_free_memory() < GROW_THRESHOLD) {
      // std::cout<<"Grow"<<std::endl;
      file->flush();
      delete file;
      bip::managed_mapped_file::grow(file_path.c_str(), GROW_CHUNK);
      this->reload_file();
    }
  }

  void Index::reload_file() {
    try {
      auto size = std::experimental::filesystem::file_size(file_path.c_str());
      file = new bip::managed_mapped_file(bip::open_or_create, file_path.c_str(), size);
    } catch(...) {
      file = new bip::managed_mapped_file(bip::open_or_create, file_path.c_str(), 1048576);
    }

    void_alloc alloc(file->get_segment_manager());
    map = file->find_or_construct<index_map>("index")(std::less<IndexKey>(), alloc);
  }

  std::optional<IndexValue> Index::get(const IndexKey &key) {
    auto it = map->find(key);
    if(it == map->end()) return {};
    return { it->second };
  }

  std::pair<IndexKey, IndexValue> Store::append(std::pair<PolarString, PolarString> val) {
    std::unique_lock space_lock(space_mut);
    auto required = val.second.size();
    auto it = free_space.lower_bound({ required, { -1, -1 }});

    /*
    for(const auto &inner : free_space)
      std::cout<<"CurFree: "<<inner.second.second<<" [] "<<inner.first<<std::endl;
    */

    size_t target_file;
    size_t target_offset;
    if(it == free_space.end()) {
      // We need a new file

      ++file_counter;
      target_file = file_counter;
      target_offset = 0;

      free_space.insert({ STORE_MAX_FILESIZE - required, { file_counter, required }});
      // std::cout<<"Initial space: "<<required<<" [] "<<STORE_MAX_FILESIZE - required<<std::endl;
    } else {
      target_file = it->second.first;
      target_offset = it->second.second;
      // std::cout<<"Using space: "<<target_offset<<" [] "<<required<<std::endl;

      auto left = it->first - required;
      free_space.erase(it);
      if(left != 0) {
        auto new_offset = target_offset + required;

        free_space.insert({ left, { target_file, new_offset }});
      }
    }

    space_lock.unlock();

    std::unique_lock lock(fs_mut);

    FILE* fd = get_fd(target_file, target_offset);

    std::fwrite(val.second.data(), sizeof(char), val.second.size(), fd);

    IndexValue value = {
      .file = target_file,
      .offset = target_offset,
      .len = val.second.size(),
    };

    auto result = std::make_pair(val.first, value);

    std::fclose(fd);

    return result;
  }

  std::string Store::fetch(const IndexValue &loc) {
    std::string result(loc.len, '\0');
    FILE* fd = get_fd(loc.file, loc.offset);
    fread(result.data(), sizeof(char), loc.len, fd);
    fclose(fd);
    return result;
  }

  void Store::free_up(const std::vector<IndexValue> &locs) {
    std::unique_lock space_lock(space_mut);
    for(const auto &loc : locs)
      free_space.insert({ loc.len, { loc.file, loc.offset }});
  }

  FILE* Store::get_fd(size_t file, size_t offset) {
    FILE* result = fopen((basedir + "/" + std::to_string(file)).c_str(), "r+");
    if(!result)
      result = fopen((basedir + "/" + std::to_string(file)).c_str(), "w+");
    fseek(result, offset, SEEK_SET);
    return result;
  }

  RetCode Engine::Open(const std::string& name, Engine** eptr) {
    return EngineRace::Open(name, eptr);
  }

  Engine::~Engine() {}

  /*
   * Complete the functions below to implement you own engine
   */

  // 1. Open engine
  RetCode EngineRace::Open(const std::string& name, Engine** eptr) {
    std::experimental::filesystem::create_directory(name);
    *eptr = NULL;
    EngineRace *engine_race = new EngineRace(name);

    *eptr = engine_race;
    return kSucc;
  }

  // 3. Write a key-value pair into engine
  RetCode EngineRace::Write(const PolarString& key, const PolarString& value) {
    auto pair = store.append({ key, value });
    journal.push(pair);
    return kSucc;
  }

  // 4. Read value of a key
  RetCode EngineRace::Read(const PolarString& key, std::string* value) {
    auto loc = journal.fetch(key);

    if(!loc) {
      std::shared_lock lock(read_lock);
      loc = { index.get(key) };
    }

    if(!loc) return kNotFound;

    *value = store.fetch(*loc);
    return kSucc;
  }

  /*
   * NOTICE: Implement 'Range' in quarter-final,
   *         you can skip it in preliminary.
   */
  // 5. Applies the given Vistor::Visit function to the result
  // of every key-value pair in the key range [first, last),
  // in order
  // lower=="" is treated as a key before all keys in the database.
  // upper=="" is treated as a key after all keys in the database.
  // Therefore the following call will traverse the entire database:
  //   Range("", "", visitor)
  RetCode EngineRace::Range(const PolarString& lower, const PolarString& upper,
      Visitor &visitor) {
    return kSucc;
  }

  template<typename C>
  void EngineRace::clear_queue(C *queue) {
    if(queue->size() == 0) return;

    std::unique_lock lock(read_lock);

    index.check_free_space();

    std::vector<std::pair<IndexKey, IndexValue>> waiting;
    waiting.reserve(queue->size());

    for(auto &[k, v] : *queue) {
      bool flag = false;
      for(auto &inner : waiting)
        if(inner.first == k) {
          inner.second = v;
          flag = true;
          break;
        }

      if(!flag) waiting.push_back({ k, v });
    }

    std::vector<IndexValue> freed;
    for(auto &[k, v] : waiting) {
      auto original = index.lossy_put(k, v);
      if(original && !(*original == v)) freed.push_back(*original);
    }
    index.persist();

    queue->clear();

    lock.unlock();

    store.free_up(freed);
  }
}  // namespace polar_race
