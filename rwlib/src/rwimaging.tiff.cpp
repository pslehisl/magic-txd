#include "StdInc.h"

#include "rwimaging.hxx"

#include "pluginutil.hxx"

#include "pixelformat.hxx"

#ifdef RWLIB_INCLUDE_TIFF_IMAGING

#include <tiff.h>
#include <tiffio.h>

#endif //RWLIB_INCLUDE_TIFF_IMAGING

namespace rw
{

#ifdef RWLIB_INCLUDE_TIFF_IMAGING

static uint16 tiff_alpha_configuration[] =
{
    EXTRASAMPLE_UNASSALPHA
};

static const imaging_filename_ext tiff_ext[] =
{
    { "TIFF", false },
    { "TIF", true }
};

// RenderWare TIFF imaging extension, because it is a great format!
// Criterion's toolchain had TIFF support, too.
struct tiffImagingExtension : public imagingFormatExtension
{
    struct tiff_uint32  { char data[4]; };
    struct tiff_uint16  { char data[2]; };
    struct tiff_uint8   { char data[1]; };

    struct tiff_number_format
    {
    private:
        bool bigEndian;

    public:
        inline tiff_number_format( bool isBigEndian )
        {
            this->bigEndian = isBigEndian;
        }

        inline uint32 get_uint32( const tiff_uint32& num ) const
        {
            if ( this->bigEndian )
            {
                return *(endian::big_endian <uint32>*)&num;
            }
            else
            {
                return *(endian::little_endian <uint32>*)&num;
            }
        }

        inline uint16 get_uint16( const tiff_uint16& num ) const
        {
            if ( this->bigEndian )
            {
                return *(endian::big_endian <uint16>*)&num;
            }
            else
            {
                return *(endian::little_endian <uint16>*)&num;
            }
        }

        inline uint8 get_uint8( const tiff_uint8& num ) const
        {
            if ( this->bigEndian )
            {
                return *(endian::big_endian <uint8>*)&num;
            }
            else
            {
                return *(endian::little_endian <uint8>*)&num;
            }
        }
    };

    struct tiff_header
    {
        char byte_order[2];
        tiff_uint16 fourtytwo;
        tiff_uint32 ifd_offset;
    };

    struct tiff_ifd_entry
    {
        tiff_uint16 tag;
        tiff_uint16 field_type;
        tiff_uint32 num_values;
        tiff_uint32 data_offset;
    };

    enum eTIFF_FieldType
    {
        FIELD_BYTE = 1,
        FIELD_ASCII,
        FIELD_SHORT,
        FIELD_LONG,
        FIELD_RATIONAL
    };

    bool IsStreamCompatible( Interface *engineInterface, Stream *inputStream ) const override
    {
        // Check whether we have a good TIFF file.
        int64 tiff_start = inputStream->tell();

        int64 tiff_size = ( inputStream->size() - tiff_start );

        tiff_header header;

        size_t headerReadCount = inputStream->read( &header, sizeof( header ) );

        if ( headerReadCount != sizeof( header ) )
        {
            return false;
        }

        // Determine whether we are little endian or big endian.
        bool is_big_endian = false;

        if ( *(unsigned short*)header.byte_order == 0x4949 )
        {
            is_big_endian = false;
        }
        else if ( *(unsigned short*)header.byte_order == 0x4D4D )
        {
            is_big_endian = true;
        }
        else
        {
            // We most likely are not a TIFF file.
            return false;
        }

        // Set up the number parser.
        tiff_number_format num( is_big_endian );

        if ( num.get_uint16( header.fourtytwo ) != 42 )
        {
            return false;
        }

        // Try to get to the image directory.
        uint32 imageFileDirectoryOffset = num.get_uint32( header.ifd_offset );

        inputStream->seek( tiff_start + imageFileDirectoryOffset, RWSEEK_BEG );

        // Now loop through all entries.
        bool hasTerminatedProperly = false;
        bool didHaveAtleastOneIFD = false;

        while ( true )
        {
            tiff_uint16 num_dir_entries;

            size_t numReadCount = inputStream->read( &num_dir_entries, sizeof( num_dir_entries ) );

            if ( numReadCount != sizeof( num_dir_entries ) )
            {
                // Something went wrong.
                return false;
            }

            uint16 dirEntryCount = num.get_uint16( num_dir_entries );

            if ( dirEntryCount == 0 )
            {
                // There cannot be an IFD without entries.
                return false;
            }

            // Read all directory entries.
            for ( uint32 n = 0; n < dirEntryCount; n++ )
            {
                tiff_ifd_entry ifd_entry;

                size_t ifdEntryReadCount = inputStream->read( &ifd_entry, sizeof( ifd_entry ) );

                if ( ifdEntryReadCount != sizeof( ifd_entry ) )
                {
                    // We cannot be aborted when reading IFD.
                    return false;
                }

                // Verify that the IFD is valid.
                size_t field_item_size = 0;

                uint16 field_type = num.get_uint16( ifd_entry.field_type );

                if ( field_type == eTIFF_FieldType::FIELD_BYTE )
                {
                    field_item_size = 1;
                }
                else if ( field_type == eTIFF_FieldType::FIELD_ASCII )
                {
                    field_item_size = 1;
                }
                else if ( field_type == eTIFF_FieldType::FIELD_SHORT )
                {
                    field_item_size = 2;
                }
                else if ( field_type == eTIFF_FieldType::FIELD_LONG )
                {
                    field_item_size = 4;
                }
                else if ( field_type == eTIFF_FieldType::FIELD_RATIONAL )
                {
                    field_item_size = 8;
                }

                // Verify the data if we known about it.
                if ( field_item_size != 0 )
                {
                    uint32 numValues = num.get_uint32( ifd_entry.num_values );

                    int64 actualDataSize = ( field_item_size * numValues );

                    // We always have this data if it is smaller than 4 bytes.
                    if ( actualDataSize > 4 )
                    {
                        // Check that we even have this data.
                        uint32 data_start = num.get_uint32( ifd_entry.data_offset );

                        bool hasTheData = ( ( data_start + actualDataSize ) <= tiff_size );

                        if ( !hasTheData )
                        {
                            // We cannot have the complete data, so bail.
                            return false;
                        }
                    }
                }
            }

            // We found a valid IFD.
            didHaveAtleastOneIFD = true;

            // We should skip to the next IFD!
            tiff_uint32 next_ifd_pointer;

            size_t ifd_pointer_readCount = inputStream->read( &next_ifd_pointer, sizeof( next_ifd_pointer ) );

            if ( ifd_pointer_readCount != sizeof( next_ifd_pointer ) )
            {
                return false;
            }

            // Jump to it.
            uint32 nextIFDPointer = num.get_uint32( next_ifd_pointer );

            // If the IFD pointer is zero, we read all of them.
            if ( nextIFDPointer == 0 )
            {
                hasTerminatedProperly = true;
                break;
            }

            inputStream->seek( tiff_start + nextIFDPointer, RWSEEK_BEG );
        }

        // Clean termination?
        if ( !hasTerminatedProperly )
        {
            return false;
        }

        // The image needs to have at least one IFD.
        if ( !didHaveAtleastOneIFD )
        {
            return false;
        }

        // We kinda qualify as a TIFF image!
        return true;
    }

    void GetStorageCapabilities( pixelCapabilities& capsOut ) const override
    {
        capsOut.supportsDXT1 = false;
        capsOut.supportsDXT2 = false;
        capsOut.supportsDXT3 = false;
        capsOut.supportsDXT4 = false;
        capsOut.supportsDXT5 = false;
        capsOut.supportsPalette = true;
    }

    struct tiff_io_struct
    {
        Interface *engineInterface;
        Stream *ioStream;
    };

    static tmsize_t TIFFReadProc( thandle_t ioptr, void *outbuf, tmsize_t count )
    {
        tiff_io_struct *io_struct = (tiff_io_struct*)ioptr;

        return io_struct->ioStream->read( outbuf, count );
    }

    static tmsize_t TIFFWriteProc( thandle_t ioptr, void *const_buf, tmsize_t writeCount )
    {
        tiff_io_struct *io_struct = (tiff_io_struct*)ioptr;

        return io_struct->ioStream->write( const_buf, writeCount );
    }

    static toff_t TIFFSeekProc( thandle_t ioptr, toff_t seekptr, int mode )
    {
        tiff_io_struct *io_struct = (tiff_io_struct*)ioptr;

        eSeekMode rwSeekMode = RWSEEK_BEG;

        if ( mode == SEEK_SET )
        {
            rwSeekMode = RWSEEK_BEG;
        }
        else if ( mode == SEEK_CUR )
        {
            rwSeekMode = RWSEEK_CUR;
        }
        else if ( mode == SEEK_END )
        {
            rwSeekMode = RWSEEK_END;
        }
        else
        {
            throw RwException( "invalid TIFF seek mode" );
        }

        Stream *rwStream = io_struct->ioStream;

        rwStream->seek( seekptr, rwSeekMode );

        return rwStream->tell();
    }

    static int TIFFCloseProc( thandle_t ioptr )
    {
        // Nothing to do here.
        return 0;
    }

    static toff_t TIFFSizeProc( thandle_t ioptr )
    {
        tiff_io_struct *io_struct = (tiff_io_struct*)ioptr;

        return io_struct->ioStream->size();
    }

    static int TIFFMapFileProc( thandle_t ioptr, void **base, toff_t *size )
    {
        // We use regular IO streams, so mapping is not required.
        return 0;
    }

    static void TIFFUnmapFileProc( thandle_t ioptr, void *base, toff_t size )
    {
        return;
    }

    inline static std::string va_to_string( const char *fmt, va_list argPtr )
    {
        int reqBufCount = _vsnprintf( NULL, 0, fmt, argPtr );

        if ( reqBufCount < 0 )
        {
            throw RwException( "libtiff format string encoding error" );
        }

        std::string msgOut;
        msgOut.resize( reqBufCount );

        // Actually process things now.
        _vsnprintf( (char*)msgOut.c_str(), reqBufCount, fmt, argPtr );

        return msgOut;
    }

    inline static std::string create_tiff_error_string( const char *whatType, const char *module, const char *fmt, va_list argPtr )
    {
        std::string full_msg( "libtiff " + std::string( whatType ) );

        if ( module )
        {
            full_msg += " (module: " + std::string( module ) + "): ";
        }
        else
        {
            full_msg += ": ";
        }

        full_msg += va_to_string( fmt, argPtr );

        return full_msg;
    }

    static void TIFFWarningHandlerExt( thandle_t ioptr, const char *module, const char *fmt, va_list argPtr )
    {
        tiff_io_struct *io_struct = (tiff_io_struct *)ioptr;

        std::string message = create_tiff_error_string( "warning", module, fmt, argPtr );

        io_struct->engineInterface->PushWarning( std::move( message ) );
    }

    static void TIFFErrorHandlerExt( thandle_t ioptr, const char *module, const char *fmt, va_list argPtr )
    {
        tiff_io_struct *io_struct = (tiff_io_struct *)ioptr;

        std::string message = create_tiff_error_string( "error", module, fmt, argPtr );

        throw RwException( std::move( message ) );
    }

    enum eTIFF_ParseMode
    {
        TPARSEMODE_GRAYSCALE,
        TPARSEMODE_FULLCOLOR,
        TPARSEMODE_PALETTE
    };

    static inline bool read_tiff_grayscale(
        const void *rowData, unsigned int index, uint16 photometric_type, uint16 bits_per_sample, bool has_alpha_channel,
        uint8& lum, uint8& alpha
    )
    {
        bool hasRead = false;

        if ( photometric_type == PHOTOMETRIC_MINISWHITE ||
             photometric_type == PHOTOMETRIC_MINISBLACK )
        {
            if ( bits_per_sample == 4 )
            {
                uint8 lumval, alphaval;

                if ( has_alpha_channel )
                {
                    struct tiff_grayscale_alpha
                    {
                        uint8 lum : 4;
                        uint8 alpha : 4;
                    };

                    const tiff_grayscale_alpha *colorData = (const tiff_grayscale_alpha*)rowData + index;

                    lumval = colorData->lum;
                    alphaval = colorData->alpha;
                }
                else
                {
                    struct tiff_grayscale_4bit
                    {
                        uint8 lum_first : 4;
                        uint8 lum_second : 4;
                    };

                    const tiff_grayscale_4bit *colorData = (const tiff_grayscale_4bit*)rowData + ( index / 2 );

                    lumval = ( ( index % 2 ) == 0 ? colorData->lum_first : colorData->lum_second );
                    alphaval = 15;
                }

                lum = ( lumval * 255 / 15 );
                alpha = ( alphaval * 255 / 15 );

                if ( photometric_type == PHOTOMETRIC_MINISWHITE )
                {
                    // We need to flip grayscale.
                    lum = 255 - lum;
                }

                hasRead = true;
            }
            else if ( bits_per_sample == 8 )
            {
                if ( has_alpha_channel )
                {
                    struct tiff_grayscale_alpha
                    {
                        uint8 lum;
                        uint8 alpha;
                    };

                    const tiff_grayscale_alpha *colorData = (const tiff_grayscale_alpha*)rowData + index;

                    lum = colorData->lum;
                    alpha = colorData->alpha;
                }
                else
                {
                    struct tiff_grayscale
                    {
                        uint8 lum;
                    };

                    const tiff_grayscale *colorData = (const tiff_grayscale*)rowData + index;

                    lum = colorData->lum;
                    alpha = 255;
                }

                hasRead = true;
            }
        }

        return hasRead;
    }

    static inline bool read_tiff_color(
        const void *rowData, unsigned int index, uint16 photometric_type, uint16 bits_per_sample, bool has_alpha_channel,
        uint8& red, uint8& green, uint8& blue, uint8& alpha
    )
    {
        bool hasRead = false;

        if ( photometric_type == PHOTOMETRIC_RGB )
        {
            if ( bits_per_sample == 8 )
            {
                if ( !has_alpha_channel )
                {
                    struct tiff_rgb
                    {
                        uint8 r, g, b;
                    };

                    const tiff_rgb *colorData = (const tiff_rgb*)rowData + index;

                    red = colorData->r;
                    green = colorData->g;
                    blue = colorData->b;
                    alpha = 0xFF;
                }
                else
                {
                    struct tiff_rgba
                    {
                        uint8 r, g, b, a;
                    };

                    const tiff_rgba *colorData = (const tiff_rgba*)rowData + index;

                    red = colorData->r;
                    green = colorData->g;
                    blue = colorData->b;
                    alpha = colorData->a;
                }

                hasRead = true;
            }
        }

        return hasRead;
    }

    void DeserializeImage( Interface *engineInterface, Stream *inputStream, imagingLayerTraversal& outputPixels ) const override
    {
        // Since the TIFF format is very complicated, I cannot guarrantee that we can read all of them.

        // Let's use our libtiff library to read us out!
        tiff_io_struct io_struct;
        io_struct.engineInterface = engineInterface;
        io_struct.ioStream = inputStream;

        TIFF *tif = TIFFClientOpen(
            "RwTIFFStreamLink_input", "r", &io_struct,
            TIFFReadProc, TIFFWriteProc, TIFFSeekProc, TIFFCloseProc, TIFFSizeProc,
            TIFFMapFileProc, TIFFUnmapFileProc
        );

        if ( tif == NULL )
        {
            throw RwException( "failed to establish TIFF I/O stream" );
        }

        try
        {
            // Obtain TIFF tags.
            uint16 photometric_type;
            uint32 image_width;
            uint32 image_length;
            uint16 bits_per_sample;
            uint16 compression;
            uint16 *colormap_red;
            uint16 *colormap_green;
            uint16 *colormap_blue;
            uint16 num_extra_samples; uint16 *extra_sample_types;
            uint16 sample_count;
            uint16 orientation;

            // Get TIFF properties.
            {
                int photometricSuccess = TIFFGetField( tif, TIFFTAG_PHOTOMETRIC, &photometric_type );

                if ( photometricSuccess != 1 )
                {
                    throw RwException( "failed to get photometric setting for TIFF" );
                }

                int imageWidthSuccess = TIFFGetField( tif, TIFFTAG_IMAGEWIDTH, &image_width );

                if ( imageWidthSuccess != 1 )
                {
                    throw RwException( "failed to get image width setting for TIFF" );
                }

                int imageLengthSuccess = TIFFGetField( tif, TIFFTAG_IMAGELENGTH, &image_length );

                if ( imageLengthSuccess != 1 )
                {
                    throw RwException( "failed to get image length setting for TIFF" );
                }

                int bitsPerSampleSuccess = TIFFGetField( tif, TIFFTAG_BITSPERSAMPLE, &bits_per_sample );

                if ( bitsPerSampleSuccess != 1 )
                {
                    throw RwException( "failed to get sample depth for TIFF" );
                }

                int compressionSuccess = TIFFGetFieldDefaulted( tif, TIFFTAG_COMPRESSION, &compression );

                if ( compressionSuccess != 1 )
                {
                    throw RwException( "failed to get compression property for TIFF" );
                }

                int colormapSuccess = TIFFGetField( tif, TIFFTAG_COLORMAP, &colormap_red, &colormap_green, &colormap_blue );

                if ( colormapSuccess != 1 )
                {
                    // We simply have no colormap.
                    colormap_red = NULL;
                    colormap_green = NULL;
                    colormap_blue = NULL;
                }

                int extrasamplesSuccess = TIFFGetField( tif, TIFFTAG_EXTRASAMPLES, &num_extra_samples, &extra_sample_types );

                if ( extrasamplesSuccess != 1 )
                {
                    // No alpha.
                    num_extra_samples = 0;
                    extra_sample_types = NULL;
                }

                int samplesPerPixelSuccess = TIFFGetField( tif, TIFFTAG_SAMPLESPERPIXEL, &sample_count );

                if ( samplesPerPixelSuccess != 1 )
                {
                    throw RwException( "failed to get the amount of samples per pixel for TIFF" );
                }

                int orientationSuccess = TIFFGetFieldDefaulted( tif, TIFFTAG_ORIENTATION, &orientation );

                if ( orientationSuccess != 1 )
                {
                    throw RwException( "failed to get the orientation property for TIFF" );
                }
            }

            // Check some obvious things.
            // We do not accept corrupted data, basically.
            if ( image_width == 0 || image_length == 0 )
            {
                throw RwException( "empty TIFF image (dimensions are zero)" );
            }

            if ( bits_per_sample == 0 )
            {
                throw RwException( "TIFF has zero sample depth" );
            }

            if ( sample_count == 0 )
            {
                throw RwException( "TIFF has no samples" );
            }

            // Determine whether this TIFF has an alpha channel.
            bool tiff_has_alpha_channel = ( num_extra_samples == 1 && ( extra_sample_types[0] == 1 || extra_sample_types[0] == 2 ) );

            // Determine to what raster format we should map to.
            eRasterFormat dstRasterFormat = RASTER_DEFAULT;
            uint32 dstDepth;
            uint32 dstRowAlignment = 4; // for good measure.
            eColorOrdering dstColorOrder = COLOR_RGBA;

            ePaletteType dstPaletteType = PALETTE_NONE;
            uint32 dstPaletteSize = 0;

            eRasterFormat tiffRasterFormat = RASTER_DEFAULT;
            uint32 tiffDepth;
            eColorOrdering tiffColorOrder = COLOR_RGBA;

            eTIFF_ParseMode parseMode;

            // TODO: allow for direct acquisition even if the orientation is off.
            if ( orientation == ORIENTATION_TOPLEFT )
            {
                if ( photometric_type == PHOTOMETRIC_MINISWHITE ||
                     photometric_type == PHOTOMETRIC_MINISBLACK )
                {
                    if ( bits_per_sample == 4 || bits_per_sample == 8 )
                    {
                        if ( num_extra_samples == 0 || tiff_has_alpha_channel )
                        {
                            if ( tiff_has_alpha_channel )
                            {
                                // We have a special new format to hold this thing.
                                dstRasterFormat = RASTER_LUM_ALPHA;
                                dstDepth = 16;

                                if ( photometric_type == PHOTOMETRIC_MINISBLACK )
                                {
                                    tiffRasterFormat = RASTER_LUM_ALPHA;
                                    tiffDepth = ( bits_per_sample * 2 );    // because we have a grayscale and an alpha sample.
                                }
                            }
                            else
                            {
                                // We store things in grayscale.
                                dstRasterFormat = RASTER_LUM;
                                dstDepth = 8;

                                if ( photometric_type == PHOTOMETRIC_MINISBLACK )
                                {
                                    tiffRasterFormat = RASTER_LUM;
                                    tiffDepth = bits_per_sample;
                                }
                            }

                            parseMode = TPARSEMODE_GRAYSCALE;
                        }
                    }
                }
                else if ( photometric_type == PHOTOMETRIC_RGB )
                {
                    if ( bits_per_sample == 8 )
                    {
                        if ( num_extra_samples == 0 || tiff_has_alpha_channel )
                        {
                            if ( tiff_has_alpha_channel )
                            {
                                dstRasterFormat = RASTER_8888;
                                dstDepth = 32;

                                tiffRasterFormat = RASTER_8888;
                                tiffDepth = 8;
                            }
                            else
                            {
                                dstRasterFormat = RASTER_888;
                                dstDepth = 24;

                                tiffRasterFormat = RASTER_888;
                                tiffDepth = 24;
                            }

                            parseMode = TPARSEMODE_FULLCOLOR;
                        }
                    }
                }
                else if ( photometric_type == PHOTOMETRIC_PALETTE )
                {
                    if ( bits_per_sample == 4 || bits_per_sample == 8 )
                    {
                        if ( colormap_red != NULL && colormap_green != NULL && colormap_blue != NULL )
                        {
                            // I am aware that palette alpha can be made on a per-texel basis here.
                            // But this would be ridiculous. Fuck that.
                            if ( num_extra_samples == 0 )
                            {
                                // We are a palette based image.
                                dstRasterFormat = RASTER_888;
                                dstDepth = bits_per_sample;

                                if ( bits_per_sample == 4 )
                                {
                                    dstPaletteType = PALETTE_4BIT;
                                }
                                else if ( bits_per_sample == 8 )
                                {
                                    dstPaletteType = PALETTE_8BIT;
                                }

                                dstPaletteSize = getPaletteItemCount( dstPaletteType );

                                parseMode = TPARSEMODE_PALETTE;

                                tiffRasterFormat = RASTER_888;
                                tiffDepth = 24;
                            }
                        }
                    }
                }
            }

            // Check whether we detected a valid raster format.
            bool hasKnownMapping = true;

            if ( dstRasterFormat == RASTER_DEFAULT )
            {
                // If not, we just set a generic one.
                dstRasterFormat = RASTER_8888;
                dstDepth = 32;
                dstColorOrder = tiffColorOrder;

                parseMode = TPARSEMODE_FULLCOLOR;

                // We most likely have no known mapping.
                hasKnownMapping = false;

                // IMPORTANT: the tiff data is expected to be directly acquired from libtiff!
            }

            // Allocate the destination texel buffer.
            // Also, if we need a palette, do that too.
            uint32 dstRowSize = getRasterDataRowSize( image_width, dstDepth, dstRowAlignment );

            uint32 dstDataSize = getRasterDataSizeByRowSize( dstRowSize, image_length );

            void *dstTexels = engineInterface->PixelAllocate( dstDataSize );

            if ( dstTexels == NULL )
            {
                throw RwException( "failed to allocate destination texel buffer in TIFF deserialization" );
            }

            try
            {
                void *dstPaletteData = NULL;

                if ( dstPaletteType != PALETTE_NONE )
                {
                    uint32 palRasterDepth = Bitmap::getRasterFormatDepth( dstRasterFormat );

                    uint32 dstPalDataSize = getPaletteDataSize( dstPaletteSize, palRasterDepth );

                    dstPaletteData = engineInterface->PixelAllocate( dstPalDataSize );

                    if ( dstPaletteData == NULL )
                    {
                        throw RwException( "failed to allocate palette data in TIFF deserialization" );
                    }
                }

                try
                {
                    // Read the texels.
                    if ( hasKnownMapping )
                    {
                        // The known mappings should be good to read as scanlines.
                        tmsize_t scanline_size = TIFFScanlineSize( tif );

                        if ( scanline_size == 0 )
                        {
                            throw RwException( "cannot read TIFF whose scanline size is zero" );
                        }

                        // Check whether we can just directly read the scanlines into our destination buffer.
                        bool canTexelsDirectlyAcquire = false;
                        bool canColorDirectlyAcquire = false;

                        if ( scanline_size == dstRowSize )
                        {
                            if ( dstPaletteType == PALETTE_NONE )
                            {
                                // We have a good chance to directly aquire the colors from raw images.
                                canColorDirectlyAcquire = 
                                    doRawMipmapBuffersNeedConversion(
                                        tiffRasterFormat, tiffDepth, tiffColorOrder, PALETTE_NONE,
                                        dstRasterFormat, dstDepth, dstColorOrder, PALETTE_NONE
                                    );

                                canTexelsDirectlyAcquire = canColorDirectlyAcquire;
                            }
                            else
                            {
                                // TIFF uses some weird palette color depth (16bits), so
                                // we definately cannot directly aquire colors.
                                canColorDirectlyAcquire = false;

                                // Let's say that we can always directly aquire the palette indice.
                                canTexelsDirectlyAcquire = true;
                            }
                        }

                        if ( canTexelsDirectlyAcquire )
                        {
                            // Just read to our destination buffer.
                            for ( uint32 row = 0; row < image_length; row++ )
                            {
                                void *dstRowData = getTexelDataRow( dstTexels, dstRowSize, row );

                                int rowReadError = TIFFReadScanline( tif, dstRowData, row );

                                if ( rowReadError != 1 )
                                {
                                    throw RwException( "failed to directly read TIFF row data" );
                                }
                            }
                        }
                        else
                        {
                            // Create a put dispatch.
                            colorModelDispatcher putDispatch( dstRasterFormat, dstColorOrder, dstDepth, NULL, 0, PALETTE_NONE );

                            void *scanlineBuf = engineInterface->PixelAllocate( scanline_size );

                            if ( scanlineBuf == NULL )
                            {
                                throw RwException( "failed to allocate scanline buffer for TIFF deserialization" );
                            }

                            try
                            {
                                // We read item by item and transform the items into the destination buffer.
                                for ( uint32 row = 0; row < image_length; row++ )
                                {
                                    int error = TIFFReadScanline( tif, scanlineBuf, row );

                                    if ( error != 1 )
                                    {
                                        throw RwException( "failed to read TIFF scanline" );
                                    }

                                    // Convert it over to our buffer.
                                    void *dstRowData = getTexelDataRow( dstTexels, dstRowSize, row );

                                    for ( uint32 col = 0; col < image_width; col++ )
                                    {
                                        if ( parseMode == TPARSEMODE_GRAYSCALE )
                                        {
                                            uint8 lum, alpha;

                                            bool hasColor = read_tiff_grayscale( scanlineBuf, col, photometric_type, bits_per_sample, tiff_has_alpha_channel, lum, alpha );

                                            if ( !hasColor )
                                            {
                                                lum = 0;
                                                alpha = 0;
                                            }

                                            putDispatch.setLuminance( dstRowData, col, lum, alpha );
                                        }
                                        else if ( parseMode == TPARSEMODE_FULLCOLOR )
                                        {
                                            uint8 r, g, b, a;

                                            bool hasColor = read_tiff_color( scanlineBuf, col, photometric_type, bits_per_sample, tiff_has_alpha_channel, r, g, b, a );

                                            if ( !hasColor )
                                            {
                                                r = 0;
                                                g = 0;
                                                b = 0;
                                                a = 0;
                                            }

                                            putDispatch.setRGBA( dstRowData, col, r, g, b, a );
                                        }
                                        else if ( parseMode == TPARSEMODE_PALETTE )
                                        {
                                            // Simple palette item copy.
                                            assert( num_extra_samples == 0 );

                                            copyPaletteItemGeneric(
                                                scanlineBuf, dstRowData,
                                                col, bits_per_sample, dstPaletteType,
                                                col, dstDepth, dstPaletteType,
                                                dstPaletteSize
                                            );
                                        }
                                        else
                                        {
                                            assert( 0 );
                                        }
                                    }
                                }
                            }
                            catch( ... )
                            {
                                engineInterface->PixelFree( scanlineBuf );

                                throw;
                            }

                            engineInterface->PixelFree( scanlineBuf );
                        }

                        // Copy palette colors.
                        if ( dstPaletteType != PALETTE_NONE )
                        {
                            // Create a put dispatch.
                            uint32 palRasterDepth = Bitmap::getRasterFormatDepth( dstRasterFormat );

                            colorModelDispatcher palPutDispatch( dstRasterFormat, dstColorOrder, palRasterDepth, NULL, 0, PALETTE_NONE );

                            // Sadly, I have not found a way to retrieve the actual palette length from the TIFF.
                            // TIFF really is an outdated format. :(
                            for ( uint32 n = 0; n < dstPaletteSize; n++ )
                            {
                                uint16 red = colormap_red[ n ];
                                uint16 green = colormap_green[ n ];
                                uint16 blue = colormap_blue[ n ];
                                
                                // Convert to 8bit space.
                                // We basically scale them down linearly.
                                uint8 r = ( red * 255 / 65535 );
                                uint8 g = ( green * 255 / 65535 );
                                uint8 b = ( blue * 255 / 65535 );
                                uint8 a = 255;

                                // Store into our palette data.
                                palPutDispatch.setRGBA( dstPaletteData, n, r, g, b, a );
                            }
                        }
                    }
                    else
                    {
                        // We have got some really anonymous unknown data.
                        // To process this data we have to use the anonymous RGBA interface of libtiff.
                        // Hopefully this will solve all our problems!
                        
                        // We assume that libtiff can correctly output into our dst buffer.
                        int tiffRGBAError = TIFFReadRGBAImageOriented( tif, image_width, image_length, (uint32*)dstTexels, ORIENTATION_TOPLEFT );

                        if ( tiffRGBAError != 1 )
                        {
                            throw RwException( "failed to read virtual RGBA image data from TIFF" );
                        }
                    }

                    // Alright! We return the result texels.
                    outputPixels.layerWidth = image_width;
                    outputPixels.layerHeight = image_length;
                    outputPixels.mipWidth = image_width;
                    outputPixels.mipHeight = image_length;
                    outputPixels.texelSource = dstTexels;
                    outputPixels.dataSize = dstDataSize;

                    outputPixels.rasterFormat = dstRasterFormat;
                    outputPixels.depth = dstDepth;
                    outputPixels.rowAlignment = dstRowAlignment;
                    outputPixels.colorOrder = dstColorOrder;
                    outputPixels.paletteType = dstPaletteType;
                    outputPixels.paletteData = dstPaletteData;
                    outputPixels.paletteSize = dstPaletteSize;
                    outputPixels.compressionType = RWCOMPRESS_NONE;

                    outputPixels.hasAlpha = false;  // TODO.
                }
                catch( ... )
                {
                    // The C++ exception system is so great, because error handling dynamically stacks!
                    if ( dstPaletteData )
                    {
                        engineInterface->PixelFree( dstPaletteData );
                    }

                    throw;
                }
            }
            catch( ... )
            {
                // Since we failed, we have to release the texel buffer.
                // We aint gonna use it anymore.
                engineInterface->PixelFree( dstTexels );

                throw;
            }
        }
        catch( ... )
        {
            // Clean up the TIFF handle in case of an error.
            TIFFClose( tif );

            throw;
        }

        // Clean up resources.
        TIFFClose( tif );
    }

    void SerializeImage( Interface *engineInterface, Stream *outputStream, const imagingLayerTraversal& inputPixels ) const override
    {
        // Make sure we receive uncompressed raster data.
        if ( inputPixels.compressionType != RWCOMPRESS_NONE )
        {
            throw RwException( "cannot serialize compressed texels in TIFF serialization routine" );
        }

        // Time for some heavy and crusty data pushin'.

        // Let's use our libtiff library to read us out!
        tiff_io_struct io_struct;
        io_struct.engineInterface = engineInterface;
        io_struct.ioStream = outputStream;

        TIFF *tif = TIFFClientOpen(
            "RwTIFFStreamLink_output", "w", &io_struct,
            TIFFReadProc, TIFFWriteProc, TIFFSeekProc, TIFFCloseProc, TIFFSizeProc,
            TIFFMapFileProc, TIFFUnmapFileProc
        );

        if ( tif == NULL )
        {
            throw RwException( "failed to open TIFF RenderWare stream link for writing" );
        }

        try
        {
            uint32 width = inputPixels.mipWidth;
            uint32 height = inputPixels.mipHeight;

            const void *srcTexels = inputPixels.texelSource;

            // We have to figure out how we want to write our TIFF image.
            // Definately it should be writable directly on a scanline basis.
            eRasterFormat srcRasterFormat = inputPixels.rasterFormat;
            uint32 srcDepth = inputPixels.depth;
            uint32 srcRowAlignment = inputPixels.rowAlignment;
            eColorOrdering srcColorOrder = inputPixels.colorOrder;
            ePaletteType srcPaletteType = inputPixels.paletteType;
            const void *srcPaletteData = inputPixels.paletteData;
            uint32 srcPaletteSize = inputPixels.paletteSize;

            eRasterFormat tiffRasterFormat = RASTER_DEFAULT;
            uint32 tiffDepth;
            uint32 tiffRowAlignment = 1;
            eColorOrdering tiffColorOrder = COLOR_RGBA;

            ePaletteType tiffPaletteType = PALETTE_NONE;
            uint32 tiffPaletteSize = 0;

            // We need to set special TIFF tags aswell.
            uint16 photometric_type;
            uint16 bits_per_sample;
            uint16 sample_count;

            bool tiff_has_alpha = false;

            if ( srcPaletteType != PALETTE_NONE )
            {
                // We want to output as palette aswell.
                photometric_type = PHOTOMETRIC_PALETTE;

                if ( srcDepth == 4 || srcDepth == 8 )
                {
                    bits_per_sample = srcDepth;
                }
                else
                {
                    // We want to default to highest possible depth.
                    bits_per_sample = 8;
                }

                if ( bits_per_sample == 4 )
                {
                    tiffPaletteType = PALETTE_4BIT_LSB;
                }
                else if ( bits_per_sample == 8 )
                {
                    tiffPaletteType = PALETTE_8BIT;
                }

                tiffPaletteSize = getPaletteItemCount( tiffPaletteType );

                tiffRasterFormat = RASTER_888;
                tiffDepth = bits_per_sample;

                sample_count = 1;

                // Our palette output cannot have alpha.
                tiff_has_alpha = false;
            }
            else
            {
                tiff_has_alpha = canRasterFormatHaveAlpha( srcRasterFormat );

                eColorModel rasterColorModel = getColorModelFromRasterFormat( srcRasterFormat );

                if ( rasterColorModel == COLORMODEL_RGBA )
                {
                    photometric_type = PHOTOMETRIC_RGB;
                    bits_per_sample = 8;

                    // We have to output as in the correct raster format.
                    if ( tiff_has_alpha )
                    {
                        tiffRasterFormat = RASTER_8888;
                        tiffDepth = 32;
                    }
                    else
                    {
                        tiffRasterFormat = RASTER_888;
                        tiffDepth = 24;
                    }

                    sample_count = 3;
                }
                else if ( rasterColorModel == COLORMODEL_LUMINANCE )
                {
                    photometric_type = PHOTOMETRIC_MINISBLACK;
                    bits_per_sample = 8;

                    if ( tiff_has_alpha )
                    {
                        tiffRasterFormat = RASTER_LUM_ALPHA;
                        tiffDepth = 16;
                    }
                    else
                    {
                        tiffRasterFormat = RASTER_LUM;
                        tiffDepth = 8;
                    }

                    sample_count = 1;
                }
            }

            if ( tiffRasterFormat == RASTER_DEFAULT )
            {
                throw RwException( "could not map target raster format in TIFF serialization" );
            }

            // Determine palette things.
            void *colormap = NULL;
            uint16 *colormap_red = NULL;
            uint16 *colormap_green = NULL;
            uint16 *colormap_blue = NULL;

            if ( tiffPaletteType != PALETTE_NONE )
            {
                // Allocate one array for all palette colors.
                colormap = (uint16*)engineInterface->PixelAllocate( sizeof(uint16) * tiffPaletteSize * 3 );

                if ( colormap == NULL )
                {
                    throw RwException( "failed to allocate palette color data array for TIFF serialization" );
                }

                try
                {
                    uint32 palRasterDepth = Bitmap::getRasterFormatDepth( srcRasterFormat );

                    // Create a color dispatch for copying palette colors.
                    colorModelDispatcher palFetchDispatch( srcRasterFormat, srcColorOrder, palRasterDepth, NULL, 0, PALETTE_NONE );

                    // Partition the data into three chunks for r, g, b.
                    colormap_red = (uint16*)colormap;
                    colormap_green = colormap_red + tiffPaletteSize;
                    colormap_blue = colormap_green + tiffPaletteSize;

                    // Parse in the colors.
                    for ( uint32 n = 0; n < tiffPaletteSize; n++ )
                    {
                        uint8 r, g, b, a;

                        bool hasColor = palFetchDispatch.getRGBA( srcPaletteData, n, r, g, b, a );

                        if ( !hasColor )
                        {
                            r = 0;
                            g = 0;
                            b = 0;
                            a = 0;
                        }

                        // Scale the color values.
                        uint16 redScaled = ( r * 65535 / 255 );
                        uint16 greenScaled = ( g * 65535 / 255 );
                        uint16 blueScaled = ( b * 65535 / 255 );
                        
                        // Store stuff.
                        colormap_red[n] = redScaled;
                        colormap_green[n] = greenScaled;
                        colormap_blue[n] = blueScaled;
                    }
                }
                catch( ... )
                {
                    engineInterface->PixelFree( colormap );

                    throw;
                }
            }
            
            try
            {
                // Take care about alpha configuration.
                uint16 num_extra_samples = 0; uint16 *extra_sample_types = NULL;

                if ( tiff_has_alpha )
                {
                    // We use a simple alpha config.
                    num_extra_samples = 1;
                    extra_sample_types = tiff_alpha_configuration;
                }

                // Apply common TIFF fields.
                TIFFSetField( tif, TIFFTAG_IMAGEWIDTH, (uint16)width );
                TIFFSetField( tif, TIFFTAG_IMAGELENGTH, (uint16)height );
                TIFFSetField( tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG );
                TIFFSetField( tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE );
                TIFFSetField( tif, TIFFTAG_SAMPLESPERPIXEL, sample_count + num_extra_samples );
                TIFFSetField( tif, TIFFTAG_EXTRASAMPLES, num_extra_samples, extra_sample_types );
                TIFFSetField( tif, TIFFTAG_PHOTOMETRIC, photometric_type );
                TIFFSetField( tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample );
                TIFFSetField( tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT );

                if ( colormap_red != NULL && colormap_green != NULL && colormap_blue != NULL )
                {
                    TIFFSetField( tif, TIFFTAG_COLORMAP, colormap_red, colormap_green, colormap_blue );
                }

                // Now write our data, on a scanline-by-scanline basis.
                uint32 tiffRowSize = (uint32)TIFFScanlineSize( tif );

                uint32 srcRowSize = getRasterDataRowSize( width, srcDepth, srcRowAlignment );

                // We need to set strip size, apparently.
                TIFFSetField( tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize( tif, width * sample_count ) );

                // We first have to know if we can just directly write our data into the TIFF stream.
                // If we can, then we will do so very very fast!
                bool canDirectlyWrite = false;

                if ( tiffPaletteType != PALETTE_NONE )
                {
                    if ( tiffPaletteType == srcPaletteType && tiffDepth == srcDepth && srcRowSize == tiffRowSize )
                    {
                        canDirectlyWrite = true;
                    }
                }
                else
                {
                    canDirectlyWrite =
                        doRawMipmapBuffersNeedConversion(
                            srcRasterFormat, srcDepth, srcColorOrder, srcPaletteType,
                            tiffRasterFormat, tiffDepth, tiffColorOrder, tiffPaletteType
                        ) == false
                        &&
                        ( tiffRowSize == srcRowSize );
                }

                if ( canDirectlyWrite )
                {
                    // We just iterate through our rows and write them texels.
                    for ( uint32 row = 0; row < height; row++ )
                    {
                        const void *srcRowData = getConstTexelDataRow( srcTexels, srcRowSize, row );

                        int tiffWriteError = TIFFWriteScanline( tif, (uint32*)srcRowData, row );

                        if ( tiffWriteError != 1 )
                        {
                            throw RwException( "failed to write TIFF data row directly in serialization routine" );
                        }
                    }
                }
                else
                {
                    // We need a transformation buffer to write texels to.
                    void *rowbuf = engineInterface->PixelAllocate( tiffRowSize );

                    if ( rowbuf == NULL )
                    {
                        throw RwException( "failed to allocate TIFF auxiliary transformation row buffer in serialization routine" ); 
                    }

                    try
                    {
                        for ( uint32 row = 0; row < height; row++ )
                        {
                            // Transform our row.
                            moveTexels(
                                srcTexels, rowbuf,
                                0, row,
                                0, 0,
                                width, 1,
                                width, height,
                                srcRasterFormat, srcDepth, srcRowAlignment, srcColorOrder, srcPaletteType, srcPaletteSize,
                                tiffRasterFormat, tiffDepth, tiffRowAlignment, tiffColorOrder, tiffPaletteType, tiffPaletteSize
                            );

                            // Write the row.
                            int tiffWriteError = TIFFWriteScanline( tif, rowbuf, row );

                            if ( tiffWriteError != 1 )
                            {
                                throw RwException( "failed to write transform TIFF row in serialization routine" );
                            }
                        }
                    }
                    catch( ... )
                    {
                        engineInterface->PixelFree( rowbuf );

                        throw;
                    }

                    // Clean up some stuff that aint required anymore.
                    engineInterface->PixelFree( rowbuf );
                }
            }
            catch( ... )
            {
                // Since we do not need the palette data anymore, lets free it.
                if ( colormap )
                {
                    engineInterface->PixelFree( colormap );
                }

                throw;
            }
        }
        catch( ... )
        {
            // Clean up on error.
            TIFFClose( tif );

            throw;
        }

        // Remember to close file handles!
        TIFFClose( tif );
    }

    inline void Initialize( Interface *engineInterface )
    {
        RegisterImagingFormat( engineInterface, "Tag Image File Format", IMAGING_COUNT_EXT(tiff_ext), tiff_ext, this );
    }

    inline void Shutdown( Interface *engineInterface )
    {
        UnregisterImagingFormat( engineInterface, this );
    }
};

static PluginDependantStructRegister <tiffImagingExtension, RwInterfaceFactory_t> tiffExtensionStore;

#endif //RWLIB_INCLUDE_TIFF_IMAGING

void registerTIFFImagingExtension( void )
{
#ifdef RWLIB_INCLUDE_TIFF_IMAGING
    // Set up the libtiff library, because it uses global shit.
    TIFFSetErrorHandler( NULL );
    TIFFSetErrorHandlerExt( tiffImagingExtension::TIFFErrorHandlerExt );
    TIFFSetWarningHandler( NULL );
    TIFFSetWarningHandlerExt( tiffImagingExtension::TIFFWarningHandlerExt );

    // Register the TIFF imaging environment.
    tiffExtensionStore.RegisterPlugin( engineFactory );
#endif //RWLIB_INCLUDE_TIFF_IMAGING
}

}