# Forza Horizon Header Format

The `header` sidecar stores project metadata separately from `C_group` and
`C_livery`. The editor decodes the fields required by the project UI and retains
opaque bytes for round-trip output.

## Common Preamble

```text
u32       format version
u32       name length in UTF-16 code units
byte[]    name in UTF-16LE
u32       description length or zero
```

A zero description length selects the draft layout. A nonzero length is followed
by a UTF-16LE description and the published metadata fields.

## Draft Layout

The common date and creator fields follow the description field in both draft
and published headers. The following tail diagram describes versions 4, 6, 7,
and 9; the older layouts are described separately below:

```text
u16       year
u8        month
u8        day
byte[16]  metadata field block
byte[8]   creator identity tag
u32       creator-name length in UTF-16 code units
byte[]    creator name in UTF-16LE
byte[28]  section prefix
byte[2]   section marker
byte[]    section padding
u32?      category
u32       type value
u32?      target car identifier
byte[16]  asset GUID
byte[]    trailing data
```

FH2, FM5, and FM6 use version 0; FH3 uses version 1. After the creator name,
these headers contain a sixteen-byte prefix, a version-sized padding field,
the decal total, an optional livery car identifier, and an eight-byte asset
identifier. The asset type in the metadata field block determines whether the
car identifier is present. Remaining bytes are retained as trailing metadata.

FM7 uses version 2 with a twenty-eight-byte prefix and a one-byte `01` marker
before the decal total. Its asset identifier is sixteen bytes.
FH4 uses version 4. FH5 imports contain versions 4 and 6; the exporter selects
version 6. Both layouts use a five-byte marker-and-padding block. Their car
identifier is optional; the asset type in the metadata field
block selects it for liveries and omits it for groups. FH6 uses version 7 and a
nine-byte marker-and-padding block. Motorsport 2023 uses version 9, a nine-byte
marker-and-padding block, and a category preceding the type value. Its car
identifier follows the type value.

The metadata field block, creator identity tag, section prefix, marker padding, GUID, and trailing
data are preserved by the editor. Livery export synchronizes the target car
identifier and decal total with the embedded container metadata.
Trailing metadata has variable length and can contain repeated identity and
count fields. Its presence and length do not determine the core header layout.

## Published Layout

Published headers use the same metadata fields after a non-empty description.
Their section prefix can contain state and repeated identity data and is retained
unchanged.

## Codec Behavior

`parseHeader()` validates string and fixed-field bounds before returning
`HeaderMetadata`. `buildHeader()` writes the corresponding draft or published
layout. A new draft receives format version 7, the current date fields, a new
GUID, and initialized opaque blocks.

Imported header bytes remain attached to the project. Group export converts
published metadata to a draft header, clears the description, synchronizes the
decal total, and uses the destination save-profile identity before writing the
output folder. Native target export selects the destination layout, normalizes
publication metadata, and retains compatible identity fields. Opaque fields
without a compatible destination representation receive the target defaults.
Projects without an imported header receive a generated draft header.
