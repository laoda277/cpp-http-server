#include "FileCache.h"
#include <sys/stat.h>

FileCache::FileCache(size_t maxBytes): maxBytes_(maxBytes){}

std::shared_ptr<const std::string> FileCache::get(const std::string& path){
    std::shared_ptr<CacheEntry> entry;
    {
       std::lock_guard<std::mutex> lk(cacheMutex_);
       auto it = map_.find(path);  //map中找对应文件
       if(it == map_.end()) return nullptr;

       entry = it->second; //拷贝一份shared_ptr（计数+1），锁外使用期间保证条目存活
       lru_.splice(lru_.begin(), lru_, entry->lruIt); //通过链表迭代器将项的位置置于表头
    entry->lruIt = lru_.begin();   // 同步位置凭证
    }

    struct stat st; //存储文件属性的结构体
    if(stat(path.c_str(), &st) != 0 || st.st_mtime != entry->mtime){ //获取磁盘上的文件属性  非零说明文件丢失||缓存时间戳与硬盘时间戳不相等说明硬盘上的被修改了
        std::lock_guard<std::mutex> lk(cacheMutex_);
        removeEntryLocked(path, entry.get()); //get是共享指针的内部函数 返回实际对象的裸指针 除此之外还有一个计数指针
        return nullptr;
    }

    return {entry, &entry->content}; //别名构造 根据函数签名的返回值判定是shared_ptr的构造，
                                     // 第一个参数是指针的控制块（生命周期与引用计数）， 第二个参数是指向的地址
}

    void FileCache::put(const std::string& path, std::string content){
        if(content.size() > kMaxFileToCache) return ;

        struct stat st;
        if(stat(path.c_str(), &st) != 0) return;//文件丢失

        std::lock_guard<std::mutex> lk(cacheMutex_);

        auto it = map_.find(path);
        if(it != map_.end()){
          removeEntryLocked(path, it->second.get()); //已存在的统一删除
        }

        usedBytes_ += content.size();
        lru_.push_front(path); //路径排序顺序放最前
        auto entry = std::make_shared<CacheEntry> (); //创建新节点并绑定
        entry->content = std::move(content);
        entry->mtime = st.st_mtime;
        entry->lruIt = lru_.begin();
        map_[path] = std::move(entry);

        evictIfNeeded();
    }

    void FileCache::removeEntryLocked(const std::string& path, const CacheEntry* expected){
        auto it = map_.find(path);
        if(it == map_.end() || it->second.get() != expected) return;

        usedBytes_ -= it->second->content.size();
        lru_.erase(it->second->lruIt);
        map_.erase(it);
    }

    void FileCache::evictIfNeeded(){
        while(usedBytes_ > maxBytes_ && !lru_.empty()){
            const std::string victim = lru_.back();
            auto it = map_.find(victim);
            usedBytes_ -= it->second->content.size();
            map_.erase(it);
            lru_.pop_back();
        }
    }

