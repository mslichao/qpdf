#include <qpdf/Pl_Flate.hh>

#include <climits>
#include <cstring>
#include <zlib.h>

#include <qpdf/QIntC.hh>
#include <qpdf/QUtil.hh>

namespace
{
    unsigned long long memory_limit{0};
} // namespace

int Pl_Flate::compression_level = Z_DEFAULT_COMPRESSION;

Pl_Flate::Members::Members(size_t out_bufsize, action_e action) :
    out_bufsize(out_bufsize),
    action(action),
    initialized(false),
    zdata(nullptr)
{
    this->outbuf = QUtil::make_shared_array<unsigned char>(out_bufsize);
    // Indirect through zdata to reach the z_stream so we don't have to include zlib.h in
    // Pl_Flate.hh.  This means people using shared library versions of qpdf don't have to have zlib
    // development files available, which particularly helps in a Windows environment.
    this->zdata = new z_stream;

    if (out_bufsize > UINT_MAX) {
        throw std::runtime_error(
            "Pl_Flate: zlib doesn't support buffer sizes larger than unsigned int");
    }

    z_stream& zstream = *(static_cast<z_stream*>(this->zdata));
    zstream.zalloc = nullptr;
    zstream.zfree = nullptr;
    zstream.opaque = nullptr;
    zstream.next_in = nullptr;
    zstream.avail_in = 0;
    zstream.next_out = this->outbuf.get();
    zstream.avail_out = QIntC::to_uint(out_bufsize);
}

Pl_Flate::Members::~Members()
{
    if (this->initialized) {
        z_stream& zstream = *(static_cast<z_stream*>(this->zdata));
        if (action == a_deflate) {
            deflateEnd(&zstream);
        } else {
            inflateEnd(&zstream);
        }
    }

    delete static_cast<z_stream*>(this->zdata);
    this->zdata = nullptr;
}

Pl_Flate::Pl_Flate(
    char const* identifier, Pipeline* next, action_e action, unsigned int out_bufsize_int) :
    Pipeline(identifier, next),
    m(new Members(QIntC::to_size(out_bufsize_int), action))
{
}

Pl_Flate::~Pl_Flate() // NOLINT (modernize-use-equals-default)
{
    // Must be explicit and not inline -- see QPDF_DLL_CLASS in README-maintainer
}

void
Pl_Flate::setMemoryLimit(unsigned long long limit)
{
    memory_limit = limit;
}

void
Pl_Flate::setWarnCallback(std::function<void(char const*, int)> callback)
{
    m->callback = callback;
}

void
Pl_Flate::warn(char const* msg, int code)
{
    if (m->callback != nullptr) {
        m->callback(msg, code);
    }
}

void
Pl_Flate::write(unsigned char const* data, size_t len)
{
    if (m->outbuf == nullptr) {
        throw std::logic_error(
            this->identifier + ": Pl_Flate: write() called after finish() called");
    }

    // Write in chunks in case len is too big to fit in an int. Assume int is at least 32 bits.
    static size_t const max_bytes = 1 << 30;
    size_t bytes_left = len;
    unsigned char const* buf = data;
    while (bytes_left > 0) {
        size_t bytes = (bytes_left >= max_bytes ? max_bytes : bytes_left);
        handleData(buf, bytes, (m->action == a_inflate ? Z_SYNC_FLUSH : Z_NO_FLUSH));
        bytes_left -= bytes;
        buf += bytes;
    }
}

void
Pl_Flate::handleData(unsigned char const* data, size_t len, int flush)
{
    if (len > UINT_MAX) {
        throw std::runtime_error("Pl_Flate: zlib doesn't support data blocks larger than int");
    }
    z_stream& zstream = *(static_cast<z_stream*>(m->zdata));
    // zlib is known not to modify the data pointed to by next_in but doesn't declare the field
    // value const unless compiled to do so.
    zstream.next_in = const_cast<unsigned char*>(data);
    zstream.avail_in = QIntC::to_uint(len);

    if (!m->initialized) {
        int err = Z_OK;

        // deflateInit and inflateInit are macros that use old-style casts.
#if ((defined(__GNUC__) && ((__GNUC__ * 100) + __GNUC_MINOR__) >= 406) || defined(__clang__))
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (m->action == a_deflate) {
            err = deflateInit(&zstream, compression_level);
        } else {
            err = inflateInit(&zstream);
        }
#if ((defined(__GNUC__) && ((__GNUC__ * 100) + __GNUC_MINOR__) >= 406) || defined(__clang__))
# pragma GCC diagnostic pop
#endif

        checkError("Init", err);
        m->initialized = true;
    }

    int err = Z_OK;

    bool done = false;
    while (!done) {
        if (m->action == a_deflate) {
            err = deflate(&zstream, flush);
        } else {
            err = inflate(&zstream, flush);
        }
        if ((m->action == a_inflate) && (err != Z_OK) && zstream.msg &&
            (strcmp(zstream.msg, "incorrect data check") == 0)) {
            // Other PDF readers ignore this specific error. Combining this with Z_SYNC_FLUSH
            // enables qpdf to handle some broken zlib streams without losing data.
            err = Z_STREAM_END;
        }
        switch (err) {
        case Z_BUF_ERROR:
            // Probably shouldn't be able to happen, but possible as a boundary condition: if the
            // last call to inflate exactly filled the output buffer, it's possible that the next
            // call to inflate could have nothing to do. There are PDF files in the wild that have
            // this error (including at least one in qpdf's test suite). In some cases, we want to
            // know about this, because it indicates incorrect compression, so call a callback if
            // provided.
            this->warn("input stream is complete but output may still be valid", err);
            done = true;
            break;

        case Z_STREAM_END:
            done = true;
            // fall through

        case Z_OK:
            {
                if ((zstream.avail_in == 0) && (zstream.avail_out > 0)) {
                    // There is nothing left to read, and there was sufficient buffer space to write
                    // everything we needed, so we're done for now.
                    done = true;
                }
                uLong ready = QIntC::to_ulong(m->out_bufsize - zstream.avail_out);
                if (ready > 0) {
                    if (memory_limit && m->action != a_deflate) {
                        m->written += ready;
                        if (m->written > memory_limit) {
                            throw std::runtime_error("PL_Flate memory limit exceeded");
                        }
                    }
                    this->getNext()->write(m->outbuf.get(), ready);
                    zstream.next_out = m->outbuf.get();
                    zstream.avail_out = QIntC::to_uint(m->out_bufsize);
                }
            }
            break;

        default:
            this->checkError("data", err);
            break;
        }
    }
}

void
Pl_Flate::finish()
{
    if (m->written > memory_limit) {
        throw std::runtime_error("PL_Flate memory limit exceeded");
    }
    try {
        if (m->outbuf.get()) {
            if (m->initialized) {
                z_stream& zstream = *(static_cast<z_stream*>(m->zdata));
                unsigned char buf[1];
                buf[0] = '\0';
                handleData(buf, 0, Z_FINISH);
                int err = Z_OK;
                if (m->action == a_deflate) {
                    err = deflateEnd(&zstream);
                } else {
                    err = inflateEnd(&zstream);
                }
                m->initialized = false;
                checkError("End", err);
            }

            m->outbuf = nullptr;
        }
    } catch (std::exception& e) {
        try {
            this->getNext()->finish();
        } catch (...) {
            // ignore secondary exception
        }
        throw std::runtime_error(e.what());
    }
    this->getNext()->finish();
}

void
Pl_Flate::setCompressionLevel(int level)
{
    compression_level = level;
}

void
Pl_Flate::checkError(char const* prefix, int error_code)
{
    z_stream& zstream = *(static_cast<z_stream*>(m->zdata));
    if (error_code != Z_OK) {
        char const* action_str = (m->action == a_deflate ? "deflate" : "inflate");
        std::string msg = this->identifier + ": " + action_str + ": " + prefix + ": ";

        if (zstream.msg) {
            msg += zstream.msg;
        } else {
            switch (error_code) {
            case Z_ERRNO:
                msg += "zlib system error";
                break;

            case Z_STREAM_ERROR:
                msg += "zlib stream error";
                break;

            case Z_DATA_ERROR:
                msg += "zlib data error";
                break;

            case Z_MEM_ERROR:
                msg += "zlib memory error";
                break;

            case Z_BUF_ERROR:
                msg += "zlib buffer error";
                break;

            case Z_VERSION_ERROR:
                msg += "zlib version error";
                break;

            default:
                msg += std::string("zlib unknown error (") + std::to_string(error_code) + ")";
                break;
            }
        }

        throw std::runtime_error(msg);
    }
}
