#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

// Copy each NUL-terminated source in `files` into a new buffer with '#'
// comments removed and pseudo-ops expanded; `paths` name the files in
// diagnostics. Each output starts with a NUL sentinel byte (the text begins
// at out[i] + 1). Returns NULL after reporting an error, having freed every
// partial output; on success the caller frees each buffer and the array.
char** preprocess(int num_files, const char* const* paths, const char* const* files);

#endif  // PREPROCESSOR_H
