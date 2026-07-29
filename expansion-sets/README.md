# QuickType common expansion sets

`catalog.json` lists the sets shown by the QuickType configurator. Each catalog
entry points to a versioned `quicktype-set/v2` JSON file in this directory.
Set `"required": true` on a catalog entry to make the configurator automatically
check and install changed contents whenever a device connects.

Sets can be created, imported, and edited from the configurator's Common Expansions
screen while no device is connected. The normal typed-expansion, keypad, and
placeholder editors are reused for set contents. **Export Common Set** creates
the single JSON file to publish; the editor handles its internal structure.

To publish an update:

1. Open the set with **Edit Set** (or **Edit Local Set** for a local file).
2. Edit its expansions and metadata, then choose **Export Common Set**.
3. Replace the set's existing server file with the export using the hosting
   system's normal upload method.

Set and expansion IDs are stable identifiers. Do not reuse an existing ID for a
different set or expansion. A v2 set contains `typedExpansions`,
`keypadExpansions`, and placeholder definitions. The configurator accepts at
most 128 total active typed and keypad rules and validates the device's storage,
trigger, and template limits before applying it. Legacy v1 files remain
importable and are treated as typed-only profiles.

Installed common rules are marked with their set and stable entry IDs. They are
read-only in the ordinary device editor, but can be cloned into an editable
personal rule. Applying an update replaces only rules previously managed by
that common set. Personal typed expansions, unrelated keypad assignments,
placeholder values, device name, and color are preserved. The configurator
compares actual set contents, so updates are detected even if a publisher
forgets to increment the informational revision number.

When connected, **Install on Device** writes the merged configuration and reads
it back before reporting success. When disconnected, **Apply Set** performs the
same merge in the browser editor.
