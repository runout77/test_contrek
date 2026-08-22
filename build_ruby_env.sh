#!/bin/bash

set -e

bundle install

POLICY_FILE=$(find /etc/ImageMagick* -name policy.xml | head -n 1)
if [ -z "$POLICY_FILE" ]; then
  echo "Error: policy.xml not found!"
  exit 1
fi
cat <<EOF > "$POLICY_FILE"
<policymap>
  <policy domain="resource" name="memory" value="10GiB"/>
  <policy domain="resource" name="map" value="10GiB"/>
  <policy domain="resource" name="width" value="64KP"/>
  <policy domain="resource" name="height" value="64KP"/>
  <policy domain="resource" name="area" value="2GiB"/>
  <policy domain="resource" name="disk" value="16GiB"/>
  <policy domain="coder" rights="read | write" pattern="{PNG,JPEG,SVG,PPM}" />
</policymap>
EOF