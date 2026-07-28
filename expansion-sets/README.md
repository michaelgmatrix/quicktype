# QuickType shared expansion sets

`catalog.json` lists the sets shown by the QuickType configurator. Each catalog
entry points to a versioned `quicktype-set/v1` JSON file in this directory.

To publish an update:

1. Edit the set's JSON file.
2. Increase its integer `revision`.
3. Update `updatedAt`.
4. Commit and deploy the files with the configurator.

Set and expansion IDs are stable identifiers. Do not reuse an existing ID for a
different set or expansion. The configurator accepts at most 24 expansions in a
set and validates the device's trigger and template length limits before caching
it.
