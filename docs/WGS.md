# WGS Asset Replacement

FH5 WGS export replaces an existing registered livery or layer-group container.
The export dialog accepts its active `container.N` descriptor and resolves the
save profile through the parent `containers.index`. Asset type and livery car
identifier must match the editor project. Profile ownership is derived from the
account identifier in the profile folder name.

## Storage Layout

Version-14 indices register logical container names, physical container GUIDs,
active descriptor generations, timestamps, and total blob sizes. Version-4
descriptors contain fixed-size entries with UTF-16 blob names and GUID pairs.
The current GUID identifies each physical blob file. FH5 stores its native
compressed artwork, header, and PNG thumbnails in those files.

Replacement accepts the supported index container-state value of 1.
The replacement pipeline retains the logical registration, descriptor generation,
asset GUID, blob GUID pairs, cloud revision text, and opaque index fields. It
updates the index and container timestamps and recalculates the total blob size.
The FH5 PGS encoder supplies the artwork and header; the header is assigned the
existing slot GUID and destination profile owner. Generated thumbnails replace
registered thumbnail blobs. Livery thumbnails are retained when a car preview is
unavailable and no source thumbnail is available.

## Transaction

The dialog requires confirmation that the game is closed and cloud synchronization
has finished. Export also requests confirmation of the selected replacement slot.
The writer acquires a profile-level editor lock and checks a fingerprint of the
index, descriptor, and selected blobs against the inspected destination.

Before committing, it saves the original index, descriptor, and all registered
blobs of the selected container under `.fls-wgs-backups/<backup-id>/` in the
profile folder. Backup files retain their original modification timestamps.
Each destination file is replaced through `QSaveFile`, with the index committed
last. Changes detected during the transaction abort the operation. Completed
writes are restored after a failure; a restoration error reports the retained
backup location and affected files.

To restore a backup, close the game and copy its `containers.index` and GUID
container folder over the corresponding files in the profile folder. Restore a
backup only against the matching save snapshot, because its index also contains
registrations for the rest of that profile.

New-container registration and FH6/FM23 WGS export are deferred. Game acceptance
and cloud synchronization require user testing on a backed-up save.
