# Bundling Real-ESRGAN in the packages

Automatic texture upscaling (`texauto`) runs `realesrgan-ncnn-vulkan` on the textures a game uses. It is looked
for in a `realesrgan` folder beside the program, so a package that carries it needs nothing added by hand.
The program is BSD-3-Clause (`licenses/realesrgan.license.txt`), about 40 MB per platform, so it is fetched
at packaging time and not kept in the repository.

    tools/release/fetch_realesrgan.sh OUT                      # downloads v0.2.5.0, unmodified, with licences
    REALESRGAN_DIR=OUT/linux/realesrgan tools/release/package_linux.sh ...
    tools/release/package_windows.ps1 -RealEsrgan OUT\windows\realesrgan

Without the folder the packages are as before and the feature says the tool is missing.
