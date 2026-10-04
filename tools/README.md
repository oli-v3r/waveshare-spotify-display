# Local Spotify authorization

Run `node tools/get-spotify-refresh-token.mjs` from the repository root to
authorize Spotify on this computer and obtain a replacement refresh token.
The script reads the Client ID and Client Secret from the Git-ignored
`include/secrets.h`, uses a loopback-only callback at
`http://127.0.0.1:8888/callback`, and saves the token to the ignored local
secrets file without printing it.

Run `node tools/get-spotify-refresh-token.mjs --check-credentials` to verify
the app credentials without starting the browser flow. The check does not
print or save the temporary access token.

Add that exact callback URI to the Spotify app's Redirect URIs before running
the script. It requests only the `user-read-playback-state` scope and writes
the token into `SPOTIFY_REFRESH_TOKEN` in `include/secrets.h` with restrictive
file permissions. It refuses to overwrite a token already present. Never
commit or share that file.
