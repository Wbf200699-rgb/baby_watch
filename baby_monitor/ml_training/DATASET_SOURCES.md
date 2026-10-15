# Audio dataset sources and restrictions

The download script records exact Git revisions and archive checksums under
`data/raw/download_receipt.json`. Raw audio, standardized audio, manifests, and
features are intentionally excluded from Git.

| Source | Project usage | License / restriction | Canonical source |
| --- | --- | --- | --- |
| ESC-50 | Cry and non-cry detection | Full ESC-50 is CC BY-NC; ESC-10 alone is CC BY. Keep clip attribution and do not assume commercial rights. | https://github.com/karolpiczak/ESC-50 |
| Donate-a-Cry | Cry detection and contributor-reported reason labels | Database: ODbL 1.0; individual contents: DbCL 1.0. Labels are contributor suspicions, not medical confirmation. | https://github.com/gveres/donateacry-corpus |
| Infant's Cry Sound v1 | Hungry, tired, and uncomfortable/discomfort | CC BY 4.0; cite DOI 10.17632/hbppd883sd.1. | https://data.mendeley.com/datasets/hbppd883sd/1 |
| InfantCry-DBL v1 | Five DBL cry cues mapped to hungry, belly pain/lower wind, discomfort, tired, and burping/upper wind | Mendeley landing page: CC BY 4.0; cite DOI 10.17632/x493z8nmwc.1. Review upstream-source terms before commercial redistribution. | https://data.mendeley.com/datasets/x493z8nmwc/1 |
| ICSD | Optional cry detector expansion only | Gated; non-commercial, no redistribution, attribution required. The current dataset card identifies CC BY-NC-SA 4.0. User approval and a private token are required. | https://huggingface.co/datasets/QingyuLiu1/ICSD |
| Self-recorded RA8P1 audio | Domain adaptation and final held-out tests | Obtain explicit guardian consent. Store anonymous IDs only and define the intended usage in the consent record. | Private |

Do not commit raw data or access tokens. Before commercial deployment, replace
or relicense sources that prohibit commercial use and obtain legal review for
the ODbL/DbCL database obligations.

## Known label limitations

- Donate-a-Cry reasons were tagged by contributors and are not clinically
  verified.
- Mendeley v1 contains only hungry, tired, and uncomfortable. Files with the
  same inferred recording subject are grouped together to reduce leakage.
- InfantCry-DBL uses DBL acoustic-cue labels rather than clinically verified
  causes. Tier 1 has no reliable infant identity and is training-only; Tier 2
  is split by its anonymised clip/infant ID. Its v1 metadata has one documented
  filename/folder mismatch, so the pipeline deliberately trusts the folder.
- ESC-50 has only 40 `crying_baby` clips and cannot train a robust detector by
  itself.
- Public datasets do not reproduce the EK-RA8P1 microphone, enclosure, room,
  distance, or background noise. RA8P1 recordings are mandatory before release.
