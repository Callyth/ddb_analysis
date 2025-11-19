# Analysis plugin for the DeaDBeeF audio player

This project adds realtime audio analysis to the DeaDBeeF music player.
The plugin is experimental, so bugs and incomplete features are expected.

Written in C++.

## Installation

This plugin depends on Essentia.
You may need to build Essentia manually.
Tested with: [new_essentia](https://github.com/wo80/essentia)

```bash
git clone https://github.com/Callyth/ddb_analysis.git
cd ddb_analysis
# Edit Makefile and set ESSENTIA_PREFIX (default: /usr/local)
make build
make install
```

## References

- [Essentia documentation](https://essentia.upf.edu)
- [DeaDBeeF plugin guide](https://github.com/DeaDBeeF-Player/deadbeef/wiki/Developing-your-own-plugin)

## Credits

Thanks to the developers of Essentia, DeaDBeeF, Linux, and the free software community.

If you have suggestions or ideas, please open an issue.

## Screenshot

![](https://i.imgur.com/dhzysD9.png)