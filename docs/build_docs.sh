#!/bin/sh
# Build docs/protocol_spec.pdf and docs/report.pdf (report + derivations as appendix).
# Needs pandoc and xelatex; run from anywhere.
set -e
cd "$(dirname "$0")"
python3 diagrams/make_diagrams.py >/dev/null
OPTS="--pdf-engine=xelatex -V mainfont=DejaVuSerif -V sansfont=DejaVuSans
      -V monofont=DejaVuSansMono -V fontsize=10pt -V geometry:margin=2.1cm
      -V colorlinks=true -V linkcolor=blue
      -H latex-header.tex --resource-path=.:.."
pandoc protocol_spec.md $OPTS -o protocol_spec.pdf
# appendix = derivations.md without its own title line
sed '1{/^# /d}' derivations.md > /tmp/appendix.md
pandoc report.md /tmp/appendix.md $OPTS --toc -V toc-title="Contents" -o report.pdf
echo "built protocol_spec.pdf and report.pdf"
