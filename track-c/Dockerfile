FROM gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce AS build
COPY main.cpp c_join.py ./
# The scoring box is an Intel i5-13500 (Raptor Lake: AVX2, AES, PCLMUL, BMI2; no AVX-512).
# Profile-guided build: c_join.py is the published generator, byte for byte; it writes a
# training set from seed 7 (the board uses seed 1), an instrumented build runs on it, and
# the final build is optimised with that profile.
RUN F="-std=c++20 -O3 -march=raptorlake -pthread -static" && python3 c_join.py --seed 7 --scale sample --out /data \
 && mkdir /out && g++ $F -fprofile-generate -fprofile-update=atomic main.cpp -o /p && /p \
 && g++ $F -fprofile-use -fprofile-partial-training -Wno-missing-profile main.cpp -o /solution
FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
COPY --from=build /solution /app/solution
ENTRYPOINT ["/app/solution"]
