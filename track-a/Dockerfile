FROM gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce AS build
COPY main.cpp a_ingest.py ./
# Profile-guided build: train on a generated log (seed 7, not the board's seed).
RUN F="-std=c++20 -O3 -march=x86-64-v3 -flto -fno-exceptions -fno-rtti -static" && python3 a_ingest.py --seed 7 --scale full --out /t && g++ $F -fprofile-generate -fprofile-update=atomic main.cpp -o /p && /p /t /t && g++ $F -fprofile-use -fprofile-partial-training main.cpp -o /solution
FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
COPY --from=build /solution /solution
ENTRYPOINT ["/solution"]