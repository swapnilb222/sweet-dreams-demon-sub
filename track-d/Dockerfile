FROM gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce AS build
WORKDIR /src
COPY main.cpp d_traverse.py ./
# Profile-guided build, trained on a generated sample input (seed 7, not the
# board's seed). The training binary ends through exit() so it writes its profile.
RUN F="-std=c++20 -O3 -march=x86-64-v3 -DNDEBUG -Wall -Wextra -Wpedantic -pthread -static" && python3 d_traverse.py --seed 7 --scale sample --out /t && mkdir /o && g++ $F -fprofile-generate -fprofile-update=atomic -D_exit=exit main.cpp -o solution && ./solution /t /o && g++ $F -fprofile-use -fprofile-partial-training -Wno-coverage-mismatch main.cpp -o solution
FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
COPY --from=build /src/solution /app/solution
ENTRYPOINT ["/app/solution"]
