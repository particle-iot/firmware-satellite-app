# satellite-poc-fw

## Satellite Test App

A simple test app to init, connect and send messages with the Quectel BG95-S5 4G Cat-M1 / Satellite NTN radio on the Muon, with M-SoM M.2 module.

## Pre-Compiling Protobuf definitions

Ensure that the `device-os-protobuf` and `nanopb` submodules are checked out and run the build script:

```sh
git checkout main
git pull
git submodule update --init --recursive
./lib/device-os-protobuf/build.sh
```

To add a new message, check out the `constrained/sc-126020` branch in `device-os-protobuf`, add the message to the `.proto` file, update the submodule in this repo, and run the build script again.

## Cloud Build & Flash

`$ particle compile msom . --target 5.8.0 --saveTo msom-sat@5.8.0.bin; particle usb dfu; particle flash --local msom-sat@5.8.0.bin`

## Known Issues

1. 
