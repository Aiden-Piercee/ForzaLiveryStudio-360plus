# Forza Motorsport Header Format

Forza Motorsport 2023 asset folders use a `header` sidecar with the same variable-size
name, date, and creator prefix as the Horizon header. Format version 9 identifies
this dialect.

## Shared Prefix

```text
u32       format version
u32       name length in UTF-16 code units
byte[]    name in UTF-16LE
u32       description length in UTF-16 code units
byte[]    description in UTF-16LE
u16       year
u8        month
u8        day
byte[16]  metadata field block
byte[8]   creator identity tag
u32       creator-name length in UTF-16 code units
byte[]    creator name in UTF-16LE
```

The import browser uses the shared metadata fields when they can be decoded and
retains the original header bytes with the project.

## Trailing Layout

The version-9 livery tail has this structure:

```text
byte[12]  state and reserved prefix
byte[16]  asset GUID
byte[2]   marker
byte[7]   padding and state
u32       category
u32       logical decal count
u32       target car identifier
byte[16]  repeated asset GUID
byte[]    opaque trailing metadata
```

The category precedes the decal count. The fixed tail through the repeated GUID
is 65 bytes. The prefix and marker padding contain state and must not be treated
as uniformly zero reserved bytes.

The Motorsport importer treats this tail as source metadata rather than editor
project state. Livery target information is decoded from the livery data stream,
and the retained header is not regenerated during import.
