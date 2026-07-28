# QuickType shared expansion sets

`catalog.json` lists the sets shown by the QuickType configurator. Each catalog
entry points to a versioned `quicktype-set/v2` JSON file in this directory.

Sets can be created, imported, and edited from the configurator's Shared Sets
screen while no device is connected. The normal typed-expansion, keypad, and
placeholder editors are reused for set contents. **Download Set** creates the
JSON file to publish.

To publish an update:

1. Open the set with **Edit Set** (or **Import Set** for a local file).
2. Edit its expansions and metadata, then choose **Download Set**.
3. Replace the set's existing JSON file with the download.
4. Commit and deploy the files with the configurator.

Set and expansion IDs are stable identifiers. Do not reuse an existing ID for a
different set or expansion. A v2 set contains `typedExpansions`,
`keypadExpansions`, and placeholder definitions. The configurator accepts at
most 128 total active typed and keypad rules and validates the device's storage,
trigger, and template limits before applying it. Legacy v1 files remain
importable and are treated as typed-only profiles.

Applying a set replaces the complete typed and keypad expansion profile; it
does not merge rules with the existing profile. Before applying, the
configurator previews the replacement and lets the user keep device
personalization values, use the set's placeholder defaults, or edit each
placeholder. Device name and color are preserved. When connected, **Install on
Device** writes the replacement and reads it back before reporting success.
When disconnected, **Apply Set** replaces only the browser editor profile.
