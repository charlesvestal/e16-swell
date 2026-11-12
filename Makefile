# make          build/swell.e16app (needs arm-none-eabi-gcc)
# make test     the host tests (a C compiler, and Node for the editor's)
# make clean

.PHONY: all app test clean

all: app

app:
	cmake -S . -B build >/dev/null
	cmake --build build
	@echo "-> build/swell.e16app"

test:
	cmake -S test -B build-host >/dev/null
	cmake --build build-host
	build-host/swell_core_test | tail -1
	build-host/swell_fields_test | tail -1
	build-host/swell_proto_test | tail -1
	node test/editor_codec_test.mjs build-host/swell_blobdump | tail -1

clean:
	rm -rf build build-host
