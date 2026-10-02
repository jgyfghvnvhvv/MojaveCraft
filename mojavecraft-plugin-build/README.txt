Build MojaveCraft.dll with no compiler installed (GitHub Actions, free)

1. Make a free account at github.com and click New repository (private is fine).
2. On the empty repo page choose "uploading an existing file" and drag in EVERYTHING from this
   folder, including the hidden .github folder (unzip first, then drag the folders/files in).
   Make sure the path .github/workflows/build.yml is preserved.
3. Commit. Open the Actions tab: "build-plugin" runs automatically (about 2 minutes).
4. Click the finished run, scroll to Artifacts, download MojaveCraft-dll, unzip it.
5. Copy MojaveCraft.dll to <Fallout New Vegas>/Data/NVSE/Plugins (replace the old one).

If the build fails, open the failed step and copy the red error text to Claude.
