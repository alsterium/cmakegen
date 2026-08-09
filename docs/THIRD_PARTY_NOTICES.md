# Third-party notices

## SHA-256 implementation

`src/fs/safe_writer.cpp` contains a small in-tree SHA-256 implementation used
only for generated-file ownership hashes. It is an original implementation of
the algorithm specified by [FIPS PUB 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final),
which is a United States Government work. It is not copied from a third-party
source and introduces no additional third-party license.
