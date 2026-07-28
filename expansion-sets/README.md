# QuickType shared expansion sets

`catalog.json` lists the sets shown by the QuickType configurator. Each catalog
entry points to a versioned `quicktype-set/v1` JSON file in this directory.

Sets can be created, imported, and edited from the configurator's Shared Sets
screen while no device is connected. The normal expansion editor is reused for
set contents. **Download Set** creates the JSON file to publish.

To publish an update:

1. Open the set with **Edit Set** (or **Import Set** for a local file).
2. Edit its expansions and metadata, then choose **Download Set**.
3. Replace the set's existing JSON file with the download.
4. Commit and deploy the files with the configurator.

Set and expansion IDs are stable identifiers. Do not reuse an existing ID for a
different set or expansion. The configurator accepts at most 128 expansions in
a set and validates the device's shared 128-active-rule, storage, trigger, and
template limits before applying it.
