import { chmodSync, existsSync, readFileSync, renameSync, unlinkSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { randomBytes } from "node:crypto";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const secretsPath = resolve(projectRoot, "include/secrets.h");
const redirectUri = "http://127.0.0.1:8888/callback";
const scope = "user-read-playback-state";
const state = randomBytes(24).toString("hex");

function readSecret(name) {
    const contents = readFileSync(secretsPath, "utf8");
    const definition = contents.match(
        new RegExp(`^\\s*#define\\s+${name}\\s+"((?:\\\\.|[^"\\\\])*)"\\s*$`, "m")
    );
    if (!definition) {
        throw new Error(`Missing ${name} in include/secrets.h`);
    }

    const value = definition[1].replace(/\\(["\\])/g, "$1");
    if (!value) {
        throw new Error(`Set ${name} in include/secrets.h first`);
    }
    return value;
}

function saveRefreshToken(refreshToken) {
    const contents = readFileSync(secretsPath, "utf8");
    const tokenDefinition = /^(\s*#define\s+SPOTIFY_REFRESH_TOKEN\s+)"((?:\\.|[^"\\])*)"(\s*)$/m;
    const existing = contents.match(tokenDefinition);
    if (!existing) {
        throw new Error("SPOTIFY_REFRESH_TOKEN definition is missing from include/secrets.h");
    }
    if (existing[2]) {
        throw new Error("SPOTIFY_REFRESH_TOKEN is already set; refusing to overwrite it");
    }

    const updated = contents.replace(
        tokenDefinition,
        (_match, prefix, _emptyValue, suffix) => `${prefix}${JSON.stringify(refreshToken)}${suffix}`
    );
    const temporaryPath = `${secretsPath}.${process.pid}.tmp`;
    chmodSync(secretsPath, 0o600);
    try {
        writeFileSync(temporaryPath, updated, { encoding: "utf8", mode: 0o600, flag: "wx" });
        renameSync(temporaryPath, secretsPath);
    } catch (error) {
        if (existsSync(temporaryPath)) {
            unlinkSync(temporaryPath);
        }
        throw error;
    }
}

const clientId = readSecret("SPOTIFY_CLIENT_ID");
const clientSecret = readSecret("SPOTIFY_CLIENT_SECRET");

const authorizationUrl = new URL("https://accounts.spotify.com/authorize");
authorizationUrl.search = new URLSearchParams({
    client_id: clientId,
    response_type: "code",
    redirect_uri: redirectUri,
    scope,
    state
}).toString();

async function checkClientCredentials() {
    const response = await fetch("https://accounts.spotify.com/api/token", {
        method: "POST",
        headers: {
            Authorization: `Basic ${Buffer.from(`${clientId}:${clientSecret}`).toString("base64")}`,
            "Content-Type": "application/x-www-form-urlencoded"
        },
        body: new URLSearchParams({ grant_type: "client_credentials" })
    });
    const result = await response.json();

    if (!response.ok) {
        const errorCode = typeof result.error === "string" && /^[A-Za-z0-9_-]{1,80}$/.test(result.error)
            ? result.error
            : "unknown_error";
        throw new Error(`Spotify rejected the app credentials (HTTP ${response.status}, ${errorCode}).`);
    }

    console.log("Spotify accepted the app credentials. No access token was printed or saved.");
}

const server = createServer(async (request, response) => {
    const requestUrl = new URL(request.url, redirectUri);
    if (request.method !== "GET" || requestUrl.pathname !== "/callback") {
        response.writeHead(404).end("Not found");
        return;
    }

    if (requestUrl.searchParams.get("state") !== state) {
        response.writeHead(400).end("OAuth state check failed. Close this page and retry.");
        return;
    }

    const authorizationError = requestUrl.searchParams.get("error");
    if (authorizationError) {
        response.writeHead(400).end("Spotify authorization was denied. You can close this page.");
        console.error(`Spotify authorization failed: ${authorizationError}`);
        server.close();
        return;
    }

    const code = requestUrl.searchParams.get("code");
    if (!code) {
        response.writeHead(400).end("Spotify did not return an authorization code.");
        return;
    }

    try {
        const tokenResponse = await fetch("https://accounts.spotify.com/api/token", {
            method: "POST",
            headers: {
                Authorization: `Basic ${Buffer.from(`${clientId}:${clientSecret}`).toString("base64")}`,
                "Content-Type": "application/x-www-form-urlencoded"
            },
            body: new URLSearchParams({
                grant_type: "authorization_code",
                code,
                redirect_uri: redirectUri
            })
        });

        const tokens = await tokenResponse.json();
        if (!tokenResponse.ok || typeof tokens.refresh_token !== "string" || !tokens.refresh_token) {
            const errorCode = typeof tokens.error === "string" && /^[A-Za-z0-9_-]{1,80}$/.test(tokens.error)
                ? tokens.error
                : "unknown_error";
            throw new Error(
                `Spotify token exchange failed (HTTP ${tokenResponse.status}, ${errorCode}). ` +
                "Verify the app's Client ID, newly rotated Client Secret, and exact Redirect URI."
            );
        }

        saveRefreshToken(tokens.refresh_token);
        response.writeHead(200, { "Content-Type": "text/plain; charset=utf-8" });
        response.end("Spotify authorization succeeded. The refresh token was saved to your ignored local secrets file.");
        console.log("\nSpotify authorization succeeded; the refresh token was saved to ignored include/secrets.h.");
        server.close();
    } catch (error) {
        response.writeHead(500, { "Content-Type": "text/plain; charset=utf-8" });
        response.end("Token exchange failed. Check the terminal for details.");
        console.error(`Could not save or exchange the refresh token: ${error.message}`);
        server.close();
        process.exitCode = 1;
    }
});

server.on("error", (error) => {
    console.error(`Could not start the local callback server: ${error.message}`);
    process.exitCode = 1;
});

if (process.argv.includes("--check-credentials")) {
    checkClientCredentials().catch((error) => {
        console.error(error.message);
        process.exitCode = 1;
    });
} else {
    server.listen(8888, "127.0.0.1", () => {
        console.log(`Listening for Spotify's callback at ${redirectUri}.`);
        console.log("Opening the Spotify authorization page in your browser...");
        const browser = spawn("open", [authorizationUrl.toString()], { stdio: "ignore" });
        browser.on("error", () => {
            console.log(`Open this URL in your browser:\n${authorizationUrl}`);
        });
    });
}
