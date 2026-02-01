#!/usr/bin/env zsh

start_path=$(pwd)
/opt/homebrew/bin/doxygen Doxyfile
rm -r ~/Developer/jonnybergdahl/jonnybergdahl.github.io/jbhamqttdiscovery/*
cp -r docs/* ~/Developer/jonnybergdahl/jonnybergdahl.github.io/jbhamqttdiscovery

# Commit and push docs
cd ~/Developer/jonnybergdahl/jonnybergdahl.github.io || exit
git checkout main
git add jbhamqttdiscovery
if [ -n "$(git status --porcelain)" ]; then
  git commit -m "Update JBHaMqttDiscovery docs"
  git push
else
   echo "Branch main is up to date,nothing to do."
fi
cd "$start_path" || exit
