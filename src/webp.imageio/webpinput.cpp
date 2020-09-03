// Copyright 2008-present Contributors to the OpenImageIO project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/OpenImageIO/oiio/blob/master/LICENSE.md

#include <cstdio>

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imageio.h>

#include <webp/decode.h>
#include <webp/demux.h>

OIIO_PLUGIN_NAMESPACE_BEGIN

namespace webp_pvt {


class WebpInput final : public ImageInput {
public:
    WebpInput() { init(); }
    virtual ~WebpInput() { close(); }
    virtual const char* format_name() const override { return "webp"; }
    virtual bool open(const std::string& name, ImageSpec& spec) override;
    virtual bool seek_subimage(int subimage, int miplevel) override;
    virtual bool read_native_scanline(int subimage, int miplevel, int y, int z,
                                      void* data) override;
    virtual int current_subimage(void) const override
    {
        lock_guard lock(m_mutex);
        return m_subimage;
    }
    virtual bool close() override;

private:
    std::string m_filename;

    WebPDemuxer* m_demux; // Demuxer object
    WebPIterator m_iter;  // Frame iterator

    int m_subimage;  // What subimage are we looking at?

    uint8_t* m_decoded_image;
    uint64_t m_image_size;
    long int m_scanline_size;
    FILE* m_file;

    void init()
    {
        m_demux         = NULL;
        m_image_size    = 0;
        m_scanline_size = 0;
        m_decoded_image = NULL;
        m_file          = NULL;
        m_subimage      = -1;
    }
};


bool
WebpInput::open(const std::string& name, ImageSpec& spec)
{
    m_filename = name;

    // Perform preliminary test on file type.
    if (!Filesystem::is_regular(m_filename)) {
        errorf("Not a regular file \"%s\"", m_filename);
        return false;
    }

    // Get file size and check we've got enough data to decode WebP.
    m_image_size = Filesystem::file_size(name);
    if (m_image_size == uint64_t(-1)) {
        errorf("Failed to get size for \"%s\"", m_filename);
        return false;
    }
    if (m_image_size < 12) {
        errorf("File size is less than WebP header for file \"%s\"",
               m_filename);
        return false;
    }

    m_file = Filesystem::fopen(m_filename, "rb");
    if (!m_file) {
        errorf("Could not open file \"%s\"", m_filename);
        return false;
    }

    // Read header and verify we've got WebP image.
    std::vector<uint8_t> image_header;
    image_header.resize(std::min(m_image_size, (uint64_t)64), 0);
    size_t numRead = fread(&image_header[0], sizeof(uint8_t),
                           image_header.size(), m_file);
    if (numRead != image_header.size()) {
        errorf("Read failure for header of \"%s\" (expected %d bytes, read %d)",
               m_filename, image_header.size(), numRead);
        close();
        return false;
    }

    int width = 0, height = 0;
    if (!WebPGetInfo(&image_header[0], image_header.size(), &width, &height)) {
        errorf("%s is not a WebP image file", m_filename);
        close();
        return false;
    }

    // Read actual data and decode.
    std::vector<uint8_t> encoded_image;
    encoded_image.resize(m_image_size, 0);
    fseek(m_file, 0, SEEK_SET);
    numRead = fread(&encoded_image[0], sizeof(uint8_t), encoded_image.size(),
                    m_file);
    if (numRead != encoded_image.size()) {
        errorf("Read failure for \"%s\" (expected %d bytes, read %d)",
               m_filename, encoded_image.size(), numRead);
        close();
        return false;
    }

    WebPData data;
    data.bytes = &encoded_image[0];
    data.size  = m_image_size;

    m_demux = WebPDemux(&data);
    if (m_demux == nullptr) {
        errorf("Failed to parse %s file", m_filename);
        //close();
        return false;
    }
    uint32_t canvas_width  = WebPDemuxGetI(m_demux, WEBP_FF_CANVAS_WIDTH);
    uint32_t canvas_height = WebPDemuxGetI(m_demux, WEBP_FF_CANVAS_HEIGHT);

    const int nchannels = 4;
    m_scanline_size     = width * nchannels;
    m_spec = ImageSpec(canvas_width, canvas_height, nchannels, TypeDesc::UINT8);
    m_spec.attribute("oiio:ColorSpace", "sRGB");  // webp is always sRGB
    spec = m_spec;

    if (!seek_subimage(0, 0)) {
        return false;
    }

    if (m_iter.num_frames > 1) {
        m_spec.attribute("oiio:Movie", 1); // mark as animated

        int delay  = m_iter.duration;
        int rat[2] = { 1000, delay };
        m_spec.attribute("FramesPerSecond", TypeRational, &rat);
    }

    // WebP requires unassociated alpha, and it's sRGB.
    // Handle this all by wrapping an IB around it.
    ImageSpec specwrap(m_spec.width, m_spec.height, 4, TypeUInt8);
    ImageBuf bufwrap(specwrap, m_decoded_image);
    ROI rgbroi(0, m_spec.width, 0, m_spec.height, 0, 1, 0, 3);
    ImageBufAlgo::pow(bufwrap, bufwrap, 2.2f, rgbroi);
    ImageBufAlgo::premult(bufwrap, bufwrap);
    ImageBufAlgo::pow(bufwrap, bufwrap, 1.0f / 2.2f, rgbroi);

    return true;
}

bool
WebpInput::seek_subimage(int subimage, int miplevel)
{
    if (subimage < 0 || miplevel != 0)
        return false;

    if (m_subimage == subimage) {
        // We're already pointing to the right subimage
        return true;
    }

    int frame_num = subimage + 1; // oiio subimages are 0 based, webp frames start with 1
    if (WebPDemuxGetFrame(m_demux, frame_num, &m_iter)) {
        //m_spec.width       = m_gif_file->SWidth;
        //m_spec.height      = m_gif_file->SHeight;
        //m_spec.depth       = 1;
        //m_spec.full_height = m_spec.height;
        //m_spec.full_width  = m_spec.width;
        //m_spec.full_depth  = m_spec.depth;
        int width;
        int height;
        m_subimage = subimage;

        if (!(m_decoded_image = WebPDecodeRGBA(m_iter.fragment.bytes,
                                               m_iter.fragment.size, &width,
                                               &height))) {
            errorf("Couldn't decode %s on frame %i", m_filename, frame_num);
            close();
            return false;
        }
        return true;
    }

    return false;
}


bool
WebpInput::read_native_scanline(int subimage, int miplevel, int y,
                                int /*z*/, void* data)
{
    lock_guard lock (m_mutex);
    if (! seek_subimage (subimage, miplevel))
         return false;

    if (y < 0 || y >= m_spec.height)  // out of range scanline
        return false;
    memcpy(data, &m_decoded_image[y * m_scanline_size], m_scanline_size);
    return true;
}


bool
WebpInput::close()
{
    if (m_file) {
        fclose(m_file);
        m_file = NULL;
    }
    if (m_decoded_image) {
        free(m_decoded_image);
        m_decoded_image = NULL;
    }
    WebPDemuxReleaseIterator(&m_iter);
    if (m_demux) {
        WebPDemuxDelete(m_demux);
    }
    return true;
}

}  // namespace webp_pvt

// Obligatory material to make this a recognizeable imageio plugin
OIIO_PLUGIN_EXPORTS_BEGIN

OIIO_EXPORT int webp_imageio_version = OIIO_PLUGIN_VERSION;

OIIO_EXPORT const char*
webp_imageio_library_version()
{
    int v = WebPGetDecoderVersion();
    return ustring::sprintf("Webp %d.%d.%d", v >> 16, (v >> 8) & 255, v & 255)
        .c_str();
}

OIIO_EXPORT ImageInput*
webp_input_imageio_create()
{
    return new webp_pvt::WebpInput;
}

OIIO_EXPORT const char* webp_input_extensions[] = { "webp", nullptr };

OIIO_PLUGIN_EXPORTS_END

OIIO_PLUGIN_NAMESPACE_END
