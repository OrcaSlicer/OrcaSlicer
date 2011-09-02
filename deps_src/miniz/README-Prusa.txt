This library is based on miniz 2.1.0 - amalgamated version.

----------------------------------------------------------------

PrusaResearch (Vojtech) homebrewed the following:

mz_zip_writer_add_staged_open(), mz_zip_writer_add_staged_data() and mz_zip_writer_add_staged_finish() 
were derived from mz_zip_writer_add_read_buf_callback() by splitting it and passing a new
mz_zip_writer_staged_context between them.

----------------------------------------------------------------

Merged with https://github.com/richgel999/miniz/pull/147
to support writing a zipped file using a callback without
knowing the size of the file up front.

Vojtech made the following comments to the pull request above:

The pull request looks good to me in general. We need such a functionality at https://github.com/prusa3d/PrusaSlicer so we are going to merge it into our project. Namely, we are exporting potentially huge model files, which are XML encoded, thus the compression ration is quite high and keeping the XML encoded model file in memory is not really practical.

I am a bit uneasy about two things though:

mz_zip_writer_create_zip64_extra_data() call at the start of the file block stores uncompressed and compressed size. Naturally the uncompressed size will be over estimated, while the compressed size will be zero (as it was before). I suppose it does not make any difference, as usually the decompressors do not read this block, but they rely on the copy of this block in the central directory at the end of the ZIP file. I used https://en.wikipedia.org/wiki/ZIP_(file_format) as a source and there is no definition of the ZIP64 extra data block, though the "Data descriptor" section says:
If the bit at offset 3 (0x08) of the general-purpose flags field is set, then the CRC-32 and file sizes are not known when the header is written. The fields in the local header are filled with zero, and the CRC-32 and size are appended in a 12-byte structure (optionally preceded by a 4-byte signature) immediately after the compressed data:

Thus I suppose it is all right if the file size at the start of the file block is not correct.

There are weird conditions in the original code as if (uncomp_size <= 3), if (uncomp_size == 0). I am not quite sure what will happen if one called your modified mz_zip_writer_add_read_buf_callback() with maximum size >= 4, but the callback provided less than 4 bytes. This is not a problem in our application, but in general it could be an issue.

Is it an issue if the maximum size mandates a 64bit format, while the callback provides less than 4GB of data? I suppose there will only be a tiny bit of performance and file size penalty, but the code will work and the exported ZIP file will be valid.

Are you using your modification in a production code? How well is your modification tested?

----------------------------------------------------------------

