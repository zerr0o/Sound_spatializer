# SADIE II data notice

The optional files downloaded by `tools/fetch-hrtf.ps1` come from the SADIE II
Database, version 2-2.

Copyright 2018 University of York. Licensed under the Apache License, Version 2.0.
Measurements were developed at the Audio Lab, University of York by Cal Armstrong,
Lewis Thresh and Gavin Kearney. The original dataset must be referenced whenever it
is used in original or modified form.

- Database: https://www.york.ac.uk/sadie-project/database.html
- Archived release: https://zenodo.org/records/12092466
- Associated paper: https://doi.org/10.3390/app8112029

The profile labels in the application are intentionally anonymous during the A/B
listening test. They do not claim that a visual face scan can identify an individual
HRTF.

The five human profiles are the seed-42 k-medoids recorded in `profiles.json`.
Their head width, height and depth were derived reproducibly from the public SADIE
II v2-2 OBJ scans and are stored in `anthropometry-v2-2.csv`; no biometric image
or mesh is shipped with the application.
