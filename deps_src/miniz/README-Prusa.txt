This library is based on miniz 3.1.2 - amalgamated version
(https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip).

----------------------------------------------------------------

Local changes

Every change to the upstream miniz.c and miniz.h is recorded in
0001-OrcaSlicer-local-changes.patch, which turns the two upstream files into
the two files in this directory. Keep the patch and the sources in step: when
one of them changes, regenerate the other.

To move to a new upstream release, unpack its miniz.c and miniz.h over the
files here and apply the patch from this directory:

    patch -p1 < 0001-OrcaSlicer-local-changes.patch

Resolve any rejects by hand, then regenerate the patch against the pristine
files (diff -u, paths a/ and b/) so the next bump starts from a clean apply.

What the patch carries:

- mz_zip_archive_file_stat.m_is_utf8, set from the UTF-8 general purpose bit
  while reading the central directory (BBS).
- mz_zip_reader_get_extra(), which returns an entry's central directory extra
  field (BBS), and mz_zip_reader_get_filename_from_extra(), which reads the
  Unicode Path extra field (0x7075) out of it (OrcaSlicer).
- mz_zip_writer_add_file_ex(), which adds a file with local and central extra
  fields; mz_zip_writer_add_file() now calls it (BBS).
- The staged writer, mz_zip_writer_add_staged_open()/_data()/_finish() and
  mz_zip_writer_staged_context, described below (PrusaResearch). The
  mz_zip_writer_add_state struct and MZ_ZIP64_MAX_CENTRAL_EXTRA_FIELD_SIZE move
  from miniz.c to miniz.h so the context can embed them.
- Local and central directory headers write version-needed 45 (4.5) and
  0xFFFFFFFF sizes when an entry needs zip64, and the central header's
  version-made-by follows version-needed.

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

