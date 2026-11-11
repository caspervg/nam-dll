# Diagonal tunnel portal generator

Builds `resources/tunnel-portal-styles/NAM_DiagonalTunnelPortals.dat`, the Road
portal pieces placed by draggable diagonal tunnels
(`src/tunnelportal/DiagonalTunnels.cpp`).

    python -I tools/diagonal-portal-gen/generate.py [output.dat] [--preview DIR]

Requires Pillow. `--preview DIR` also writes the generated textures and a
top-down plan of each piece (tile, mesh outlines per material, car lanes), as a
quick check after changing the geometry.

The script writes DBPF, S3D, FSH, SC4PATHS and exemplars itself. The S3D and
FSH layouts match `nam-mbcw-extension`'s writers. Geometry, IDs and the
coordinate frames are documented at the top of `generate.py` and in
`docs/tunnel-portal-research.md` ("Draggable diagonal tunnels").
