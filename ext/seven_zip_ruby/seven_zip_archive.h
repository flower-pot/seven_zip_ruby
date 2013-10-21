#ifndef SEVEN_ZIP_ARCHIVE_H__
#define SEVEN_ZIP_ARCHIVE_H__

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <utility>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <oleauto.h>
#include <initguid.h>
#else
#include <windows.h>
#include <CPP/Common/MyWindows.h>
#include <CPP/Common/MyInitGuid.h>
#endif

#include "guid_defs.h"


#include <ruby.h>
#include <ruby/thread.h>

#include <CPP/Common/MyCom.h>
#include <CPP/Windows/PropVariant.h>
#include <CPP/7zip/Archive/IArchive.h>
#include <CPP/7zip/IPassword.h>


#include "mutex.h"
#include "util_common.h"


namespace SevenZip
{

class ArchiveExtractCallback;

////////////////////////////////////////////////////////////////
class ArchiveBase
{
  public:
    typedef std::function<void ()> RubyAction;

    typedef std::pair<RubyAction*, bool> RubyActionTuple;

    struct RubyActionResult
    {
        int status;
        VALUE exception;

        RubyActionResult()
             : status(0), exception(Qnil)
        {
        }

        void clear()
        {
            status = 0;
            exception = Qnil;
        }

        bool isError()
        {
            return status != 0;
        }

        bool hasException()
        {
            return exception != Qnil;
        }

        void mark()
        {
            rb_gc_mark(exception);
        }
    };

  protected:
    class EventLoopThreadExecuter
    {
      public:
        EventLoopThreadExecuter(ArchiveBase *self)
             : m_self(self)
        {
            m_self->startEventLoopThread();
        }

        ~EventLoopThreadExecuter()
        {
            m_self->terminateEventLoopThread();
        }

      private:
        ArchiveBase *m_self;
    };


  public:
    ArchiveBase();
    ~ArchiveBase();
    void rubyEventLoop();
    static VALUE staticRubyEventLoop(void *p);
    void rubyWaitForAction();
    bool runRubyAction(RubyAction action);
    void finishRubyAction();
    static void cancelAction(void *p);
    virtual void cancelAction() = 0;
    static VALUE runProtectedRubyAction(VALUE p);

    template<typename T>
      void runWithoutGvl(T func);
    template<typename T, typename U>
      void runWithoutGvl(T func, U cancel);
    template<typename T>
      static void *staticRunFunctor(void *p);
    template<typename T>
      static void staticRunFunctor2(void *p);

  protected:
    void mark();
    void prepareAction();

  private:
    void startEventLoopThread();
    void terminateEventLoopThread();
    bool runRubyActionImpl(RubyAction *action);


  private:
    static RubyAction ACTION_END;

  private:
    RubyActionTuple *m_action_tuple;
    Mutex m_action_mutex;
    ConditionVariable m_action_cond_var;

    bool m_event_loop_running;

  protected:
    RubyActionResult m_action_result;
};

template<typename T>
void ArchiveBase::runWithoutGvl(T func)
{
    // TODO
    // cancelAction should be replaced with a valid function.
    rb_thread_call_without_gvl(staticRunFunctor<T>, reinterpret_cast<void*>(&func),
                               cancelAction, 0);
}

template<typename T, typename U>
void ArchiveBase::runWithoutGvl(T func, U cancel)
{
    rb_thread_call_without_gvl(staticRunFunctor<T>, reinterpret_cast<void*>(&func),
                               staticRunFunctor2<U>, reinterpret_cast<void*>(&cancel));
}

template<typename T>
void *ArchiveBase::staticRunFunctor(void *p)
{
    T *t = reinterpret_cast<T*>(p);
    (*t)();
    return 0;
}

template<typename T>
void ArchiveBase::staticRunFunctor2(void *p)
{
    T *t = reinterpret_cast<T*>(p);
    (*t)();
    return;
}

class ArchiveReader : public ArchiveBase
{
  private:
    enum ArchiveReaderState
    {
        STATE_INITIAL,
        STATE_OPENED,
        STATE_CLOSED,
        STATE_ERROR
    };


  public:
    ArchiveReader(const GUID &format_guid);
    void setProcessingStream(VALUE stream, UInt32 index, Int32 askExtractMode);
    void getProcessingStream(VALUE *stream, UInt32 *index, Int32 *askExtractMode);
    void clearProcessingStream();
    void setOperationResult(UInt32 index, Int32 result);
    VALUE callbackProc()
    {
        return m_rb_callback_proc;
    }
    void mark();
    void checkStateToBeginOperation(ArchiveReaderState expected, const std::string &msg = "Invalid operation");
    void checkState(ArchiveReaderState expected, const std::string &msg);
    virtual void cancelAction();

    // Called from Ruby script.
    VALUE open(VALUE in_stream, VALUE param);
    VALUE close();
    VALUE entryNum();
    VALUE getArchiveProperty();
    VALUE getEntryInfo(VALUE index);
    VALUE getAllEntryInfo();
    VALUE extract(VALUE index, VALUE callback_proc);
    VALUE extractFiles(VALUE index_list, VALUE callback_proc);
    VALUE extractAll(VALUE callback_proc);
    VALUE testAll(VALUE callback_proc);
    VALUE setFileAttribute(VALUE path, VALUE attrib);

    VALUE entryInfo(UInt32 index);

  private:
    ArchiveExtractCallback *createArchiveExtractCallback();
    void fillEntryInfo();

  private:
    VALUE m_rb_callback_proc;
    VALUE m_rb_out_stream;
    UInt32 m_processing_index;
    VALUE m_rb_in_stream;
    std::vector<VALUE> m_rb_entry_info_list;

    Int32 m_ask_extract_mode;
    std::vector<Int32> m_test_result;

    const GUID &m_format_guid;

    CMyComPtr<IInArchive> m_in_archive;
    CMyComPtr<IInStream> m_in_stream;

    bool m_password_specified;
    std::string m_password;

    ArchiveReaderState m_state;
};

class ArchiveWriter : public ArchiveBase
{
  private:
    enum ArchiveWriterState
    {
        STATE_INITIAL,
        STATE_OPENED,
        STATE_COMPRESSED,
        STATE_CLOSED,
        STATE_ERROR
    };


  public:
    ArchiveWriter(const GUID &format_guid);
    void mark();
    VALUE callbackProc()
    {
        return m_rb_callback_proc;
    }
    void setProcessingStream(VALUE stream, UInt32 index);
    void getProcessingStream(VALUE *stream, UInt32 *index);
    void clearProcessingStream();
    bool updateItemInfo(UInt32 index, bool *new_data, bool *new_properties, UInt32 *index_in_archive);
    VALUE itemInfo(UInt32 index)
    {
        return m_rb_update_list[index];
    }
    void checkStateToBeginOperation(ArchiveWriterState expected,
                                    const std::string &msg = "Invalid operation");
    void checkStateToBeginOperation(ArchiveWriterState expected1, ArchiveWriterState expected2,
                                    const std::string &msg = "Invalid operation");
    void checkState(ArchiveWriterState expected, const std::string &msg);
    void checkState(ArchiveWriterState expected1, ArchiveWriterState expected2,
                    const std::string &msg);
    virtual void cancelAction();

    // Called from Ruby script.
    VALUE open(VALUE out_stream, VALUE param);
    VALUE addItem(VALUE item);
    VALUE compress(VALUE callback_proc);
    VALUE close();

  protected:
    virtual HRESULT setOption(ISetProperties *set) = 0;

  private:
    VALUE m_rb_callback_proc;
    VALUE m_rb_in_stream;
    UInt32 m_processing_index;
    VALUE m_rb_out_stream;
    std::vector<VALUE> m_rb_update_list;

    const GUID &m_format_guid;

    CMyComPtr<IOutArchive> m_out_archive;
    CMyComPtr<IInStream> m_in_stream;

    bool m_password_specified;
    std::string m_password;

    ArchiveWriterState m_state;
};

////////////////////////////////////////////////////////////////
class SevenZipReader : public ArchiveReader
{
  public:
    SevenZipReader();
};

////////////////////////////////////////////////////////////////
class SevenZipWriter : public ArchiveWriter
{
  public:
    SevenZipWriter();
    virtual HRESULT setOption(ISetProperties *set);

    VALUE setMethod(VALUE method);
    VALUE method();
    VALUE setLevel(VALUE level);
    VALUE level();
    VALUE setSolid(VALUE solid);
    VALUE solid();
    VALUE setHeaderCompression(VALUE header_compression);
    VALUE headerCompression();
    VALUE setHeaderEncryption(VALUE header_encryption);
    VALUE headerEncryption();
    VALUE setMultiThreading(VALUE multi_threading);
    VALUE multiThreading();

  private:
    std::string m_method;
    UInt32 m_level;
    bool m_solid;
    bool m_header_compression;
    bool m_header_encryption;
    bool m_multi_threading;
};

////////////////////////////////////////////////////////////////
class ArchiveOpenCallback : public IArchiveOpenCallback, public ICryptoGetTextPassword,
                            public CMyUnknownImp
{
  public:
    ArchiveOpenCallback(ArchiveReader *archive);
    ArchiveOpenCallback(ArchiveReader *archive, const std::string &password);

    MY_UNKNOWN_IMP2(IArchiveOpenCallback, ICryptoGetTextPassword)

    // IArchiveOpenCallback
    STDMETHOD(SetTotal)(const UInt64 *files, const UInt64 *bytes);
    STDMETHOD(SetCompleted)(const UInt64 *files, const UInt64 *bytes);

    // ICryptoGetTextPassword
    STDMETHOD(CryptoGetTextPassword)(BSTR *password);

  protected:
    ArchiveReader *m_archive;

    bool m_password_specified;
    std::string m_password;
};

class ArchiveExtractCallback : public IArchiveExtractCallback, public ICryptoGetTextPassword,
                               public CMyUnknownImp
{
  public:
    ArchiveExtractCallback(ArchiveReader *archive);
    ArchiveExtractCallback(ArchiveReader *archive, const std::string &password);

    MY_UNKNOWN_IMP2(IArchiveExtractCallback, ICryptoGetTextPassword)

    // IProgress
    STDMETHOD(SetTotal)(UInt64 size);
    STDMETHOD(SetCompleted)(const UInt64 *completeValue);

    // IArchiveExtractCallback
    STDMETHOD(GetStream)(UInt32 index, ISequentialOutStream **outStream, Int32 askExtractMode);
    STDMETHOD(PrepareOperation)(Int32 askExtractMode);
    STDMETHOD(SetOperationResult)(Int32 resultOperationResult);

    // ICryptoGetTextPassword
    STDMETHOD(CryptoGetTextPassword)(BSTR *password);

  private:
    ArchiveReader *m_archive;

    bool m_password_specified;
    const std::string m_password;
};

class ArchiveUpdateCallback : public IArchiveUpdateCallback, public ICryptoGetTextPassword2,
                              public CMyUnknownImp
{
  public:
    ArchiveUpdateCallback(ArchiveWriter *archive);
    ArchiveUpdateCallback(ArchiveWriter *archive, const std::string &password);

    MY_UNKNOWN_IMP2(IArchiveUpdateCallback, ICryptoGetTextPassword2)

    // IProgress
    STDMETHOD(SetTotal)(UInt64 size);
    STDMETHOD(SetCompleted)(const UInt64 *completeValue);

    // IUpdateCallback
    STDMETHOD(EnumProperties)(IEnumSTATPROPSTG **enumerator);
    STDMETHOD(GetUpdateItemInfo)(UInt32 index, Int32 *newData,
                                 Int32 *newProperties, UInt32 *indexInArchive);
    STDMETHOD(GetProperty)(UInt32 index, PROPID propID, PROPVARIANT *value);
    STDMETHOD(GetStream)(UInt32 index, ISequentialInStream **inStream);
    STDMETHOD(SetOperationResult)(Int32 operationResult);

    // ICryptoGetTextPassword2
    STDMETHOD(CryptoGetTextPassword2)(Int32 *passwordIsDefined, BSTR *password);

  private:
    ArchiveWriter *m_archive;

    bool m_password_specified;
    std::string m_password;
};


class InStream : public IInStream, public CMyUnknownImp
{
  public:
    InStream(VALUE stream, ArchiveBase *archive);

    MY_UNKNOWN_IMP1(IInStream)

    STDMETHOD(Seek)(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition);
    STDMETHOD(Read)(void *data, UInt32 size, UInt32 *processedSize);

  private:
    VALUE m_stream;
    ArchiveBase *m_archive;
};


class OutStream : public IOutStream, public CMyUnknownImp
{
  public:
    OutStream(VALUE stream, ArchiveBase *archive);

    MY_UNKNOWN_IMP1(IOutStream)

    STDMETHOD(Write)(const void *data, UInt32 size, UInt32 *processedSize);

    STDMETHOD(Seek)(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition);
    STDMETHOD(SetSize)(UInt64 size);

  private:
    VALUE m_stream;
    ArchiveBase *m_archive;
};



////////////////////////////////////////////////////////////////


}

#endif
