// Copyright (c) 2012-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DBWRAPPER_H
#define BITCOIN_DBWRAPPER_H

#include <clientversion.h>
#include <fs.h>
#include <serialize.h>
#include <streams.h>
#include <util.h>
#include <utilstrencodings.h>
#include <version.h>

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

//预分配key/value的大小
static const size_t DBWRAPPER_PREALLOC_KEY_SIZE = 64;
static const size_t DBWRAPPER_PREALLOC_VALUE_SIZE = 1024;

//封装运行时出错信息
class dbwrapper_error : public std::runtime_error
{
public:
    explicit dbwrapper_error(const std::string& msg) : std::runtime_error(msg) {}
};

class CDBWrapper;

/** These should be considered an implementation detail of the specific database.
 */
namespace dbwrapper_private {

/** Handle database error by throwing dbwrapper_error exception.
 */
//通过抛出dbwrapper_error异常来处理数据库错误
void HandleError(const leveldb::Status& status);

/** Work around circular dependency, as well as for testing in dbwrapper_tests.
 * Database obfuscation should be considered an implementation detail of the
 * specific database.
 */
//获取数据库模糊秘钥
const std::vector<unsigned char>& GetObfuscateKey(const CDBWrapper &w);

};

//批量更新接口封装(一个批量更新队列)
/** Batch of changes queued to be written to a CDBWrapper */
class CDBBatch
{
    friend class CDBWrapper;

private:
    const CDBWrapper &parent;
    leveldb::WriteBatch batch;

    CDataStream ssKey;//键，流类型
    CDataStream ssValue;//值，流类型

    size_t size_estimate;//尺寸

public:
    /**
     * @param[in] _parent   CDBWrapper that this batch is to be submitted to
     */
    explicit CDBBatch(const CDBWrapper &_parent) : parent(_parent), ssKey(SER_DISK, CLIENT_VERSION), ssValue(SER_DISK, CLIENT_VERSION), size_estimate(0) { };

    void Clear()
    {
        batch.Clear();
        size_estimate = 0;
    }

    //数据写入批量读写接口，并计算尺寸
    template <typename K, typename V>
    void Write(const K& key, const V& value)
    {
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        //数据以切片形式存储
        leveldb::Slice slKey(ssKey.data(), ssKey.size());

        ssValue.reserve(DBWRAPPER_PREALLOC_VALUE_SIZE);
        ssValue << value;
        ssValue.Xor(dbwrapper_private::GetObfuscateKey(parent));
        leveldb::Slice slValue(ssValue.data(), ssValue.size());

        batch.Put(slKey, slValue);
        // LevelDB serializes writes as:
        // - byte: header　１字节
        // - varint: key length (1 byte up to 127B, 2 bytes up to 16383B, ...)　最少１字节
        // - byte[]: key    slKey.size()
        // - varint: value length　最少１字节
        // - byte[]: value  slValue.size()
        // The formula below assumes the key and value are both less than 16k.
        //下面的公式假设key和value都小于16k,因为2 bytes up to 16383B
        size_estimate += 3 + (slKey.size() > 127) + slKey.size() + (slValue.size() > 127) + slValue.size();
        ssKey.clear();
        ssValue.clear();
    }

    template <typename K>
    void Erase(const K& key)
    {
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        leveldb::Slice slKey(ssKey.data(), ssKey.size());

        batch.Delete(slKey);
        // LevelDB serializes erases as:
        // - byte: header   １字节
        // - varint: key length　最少１字节
        // - byte[]: key    slKey.size()
        // The formula below assumes the key is less than 16kB.
        size_estimate += 2 + (slKey.size() > 127) + slKey.size();
        ssKey.clear();
    }

    size_t SizeEstimate() const { return size_estimate; }
};

//leveldb迭代器接口封装
class CDBIterator
{
private:
    const CDBWrapper &parent;
    leveldb::Iterator *piter;

public:

    /**
     * @param[in] _parent          Parent CDBWrapper instance.
     * @param[in] _piter           The original leveldb iterator.
     */
    CDBIterator(const CDBWrapper &_parent, leveldb::Iterator *_piter) :
        parent(_parent), piter(_piter) { };
    ~CDBIterator();

    bool Valid() const;

    void SeekToFirst();

    //定位
    template<typename K> void Seek(const K& key) {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        //构造切片
        leveldb::Slice slKey(ssKey.data(), ssKey.size());
        piter->Seek(slKey);
    }

    void Next();

    //获取当前位置key
    template<typename K> bool GetKey(K& key) {
        leveldb::Slice slKey = piter->key();
        try {
            CDataStream ssKey(slKey.data(), slKey.data() + slKey.size(), SER_DISK, CLIENT_VERSION);
            ssKey >> key;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    //获取当前位置值
    template<typename V> bool GetValue(V& value) {
        leveldb::Slice slValue = piter->value();
        try {
            CDataStream ssValue(slValue.data(), slValue.data() + slValue.size(), SER_DISK, CLIENT_VERSION);
            ssValue.Xor(dbwrapper_private::GetObfuscateKey(parent));
            ssValue >> value;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    //获取值的尺寸
    unsigned int GetValueSize() {
        return piter->value().size();
    }

};

class CDBWrapper
{
    //友元函数
    friend const std::vector<unsigned char>& dbwrapper_private::GetObfuscateKey(const CDBWrapper &w);
private:
    //! custom environment this database is using (may be nullptr in case of default environment)
    leveldb::Env* penv;

    //! database options used
    leveldb::Options options;

    //! options used when reading from the database
    leveldb::ReadOptions readoptions;

    //! options used when iterating over values of the database
    //迭代读选项
    leveldb::ReadOptions iteroptions;

    //! options used when writing to the database
    leveldb::WriteOptions writeoptions;

    //! options used when sync writing to the database
    //同步写选项
    leveldb::WriteOptions syncoptions;

    //! the database itself
    leveldb::DB* pdb;

    //! a key used for optional XOR-obfuscation of the database
    //可选的数据库异或模糊秘钥
    std::vector<unsigned char> obfuscate_key;

    //! the key under which the obfuscation key is stored
    //模糊秘钥存储在leveldb使用的key
    static const std::string OBFUSCATE_KEY_KEY;

    //! the length of the obfuscate key in number of bytes
    //模糊秘钥的长度
    static const unsigned int OBFUSCATE_KEY_NUM_BYTES;

    //获取模糊秘钥（随机生成）
    std::vector<unsigned char> CreateObfuscateKey() const;

public:
    /**
     * @param[in] path        Location in the filesystem where leveldb data will be stored.
     * @param[in] nCacheSize  Configures various leveldb cache settings.
     * @param[in] fMemory     If true, use leveldb's memory environment.
     * @param[in] fWipe       If true, remove all existing data.
     * @param[in] obfuscate   If true, store data obfuscated via simple XOR. If false, XOR
     *                        with a zero'd byte array.
     */
    /**
     * @brief CDBWrapper
     * @param path          leveldb数据存放路径
     * @param nCacheSize    缓存设置
     * @param fMemory       是否使用leveldb的内存环境模式
     * @param fWipe         是否清空存在的数据
     * @param obfuscate     如果使能，将用模糊秘钥进行异或算法，否则使用0的字节码进行异或算法
     */
    CDBWrapper(const fs::path& path, size_t nCacheSize, bool fMemory = false, bool fWipe = false, bool obfuscate = false);
    ~CDBWrapper();

    template <typename K, typename V>
    bool Read(const K& key, V& value) const
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        //放入切片
        leveldb::Slice slKey(ssKey.data(), ssKey.size());

        std::string strValue;
        leveldb::Status status = pdb->Get(readoptions, slKey, &strValue);
        if (!status.ok()) {
            if (status.IsNotFound())
                return false;
            LogPrintf("LevelDB read failure: %s\n", status.ToString());
            dbwrapper_private::HandleError(status);
        }
        try {
            CDataStream ssValue(strValue.data(), strValue.data() + strValue.size(), SER_DISK, CLIENT_VERSION);
            ssValue.Xor(obfuscate_key);
            ssValue >> value;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    template <typename K, typename V>
    bool Write(const K& key, const V& value, bool fSync = false)
    {
        CDBBatch batch(*this);
        //先写入批处理
        batch.Write(key, value);
        //以异步模式向数据库写入更改
        return WriteBatch(batch, fSync);
    }

    //是否存在
    template <typename K>
    bool Exists(const K& key) const
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey << key;
        leveldb::Slice slKey(ssKey.data(), ssKey.size());

        std::string strValue;
        leveldb::Status status = pdb->Get(readoptions, slKey, &strValue);
        if (!status.ok()) {
            if (status.IsNotFound())
                return false;
            LogPrintf("LevelDB read failure: %s\n", status.ToString());
            dbwrapper_private::HandleError(status);
        }
        return true;
    }

    template <typename K>
    bool Erase(const K& key, bool fSync = false)
    {
        CDBBatch batch(*this);
        //先从批处理删除
        batch.Erase(key);
        //以异步模式向数据库写入更改
        return WriteBatch(batch, fSync);
    }

    bool WriteBatch(CDBBatch& batch, bool fSync = false);

    //伪接口
    // not available for LevelDB; provide for compatibility with BDB
    bool Flush()
    {
        return true;
    }

    //设置为同步更新
    bool Sync()
    {
        CDBBatch batch(*this);
        return WriteBatch(batch, true);
    }

    CDBIterator *NewIterator()
    {
        return new CDBIterator(*this, pdb->NewIterator(iteroptions));
    }

    /**
     * Return true if the database managed by this class contains no entries.
     */
    bool IsEmpty();

    template<typename K>
    size_t EstimateSize(const K& key_begin, const K& key_end) const
    {
        CDataStream ssKey1(SER_DISK, CLIENT_VERSION), ssKey2(SER_DISK, CLIENT_VERSION);
        ssKey1.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey2.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey1 << key_begin;
        ssKey2 << key_end;
        leveldb::Slice slKey1(ssKey1.data(), ssKey1.size());
        leveldb::Slice slKey2(ssKey2.data(), ssKey2.size());
        uint64_t size = 0;
        leveldb::Range range(slKey1, slKey2);
        pdb->GetApproximateSizes(&range, 1, &size);
        return size;
    }

    /**
     * Compact a certain range of keys in the database.
     */
    //压缩数据库中的一组key
    template<typename K>
    void CompactRange(const K& key_begin, const K& key_end) const
    {
        CDataStream ssKey1(SER_DISK, CLIENT_VERSION), ssKey2(SER_DISK, CLIENT_VERSION);
        ssKey1.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey2.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
        ssKey1 << key_begin;
        ssKey2 << key_end;
        leveldb::Slice slKey1(ssKey1.data(), ssKey1.size());
        leveldb::Slice slKey2(ssKey2.data(), ssKey2.size());
        pdb->CompactRange(&slKey1, &slKey2);
    }

};

#endif // BITCOIN_DBWRAPPER_H
