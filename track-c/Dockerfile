FROM gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce AS build
COPY main.cpp .
# The scoring box is an Intel i5-13500 (Raptor Lake: AVX2, AES, PCLMUL, BMI2; no AVX-512).
RUN g++ -std=c++20 -O3 -march=raptorlake -DNDEBUG -pthread -static main.cpp -o /solution
FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
COPY --from=build /solution /app/solution
ENTRYPOINT ["/app/solution"]
