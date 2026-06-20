--ea7fa4d837bedb4581dde739c114100b94d224ee7d480814edd2da3ab808
Content-Disposition: form-data; name="index.js"

var __defProp = Object.defineProperty;
var __name = (target, value) => __defProp(target, "name", { value, configurable: true });

// index.js
var index_default = {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method !== "POST") return new Response("Method not allowed", { status: 405 });
    let body;
    try {
      body = await request.json();
    } catch {
      return new Response("Invalid JSON", { status: 400 });
    }
    if (!env.KRAGWAG_SECRET || body.secret !== env.KRAGWAG_SECRET)
      return new Response("Unauthorized", { status: 401 });
    if (url.pathname === "/heartbeat") {
      if (!body.nodeId) return new Response("Missing nodeId", { status: 400 });
      const nodeId = body.nodeId.replace(/﻿/g, "").trim();
      await env.HEARTBEAT.put(`hb:${nodeId}`, String(Date.now()), { expirationTtl: 3600 });
      await env.HEARTBEAT.delete(`offline:${nodeId}`);
      return new Response("ok", { status: 200 });
    }
    if (url.pathname === "/notify") {
      if (!body.nodeId || !body.title || !body.body)
        return new Response("Missing fields: nodeId, title, body", { status: 400 });
      return await sendFCM(env, body.nodeId, body.title, body.body);
    }
    return new Response("Not found", { status: 404 });
  },
  // Cron: runs every 5 minutes — checks for stale heartbeats
  async scheduled(event, env, ctx) {
    const list = await env.HEARTBEAT.list({ prefix: "hb:" });
    const now = Date.now();
    const OFFLINE_THRESHOLD_MS = 10 * 60 * 1e3;
    for (const key of list.keys) {
      const nodeId = key.name.slice(3);
      const ts = await env.HEARTBEAT.get(key.name);
      if (!ts) continue;
      const age = now - parseInt(ts, 10);
      if (age > OFFLINE_THRESHOLD_MS) {
        const alerted = await env.HEARTBEAT.get(`offline:${nodeId}`);
        if (!alerted) {
          await sendFCM(env, nodeId, "KragWag Offline", "The fence monitor has not been seen for over 10 minutes.");
          await env.HEARTBEAT.put(`offline:${nodeId}`, "1", { expirationTtl: 86400 });
        }
      }
    }
  }
};
async function sendFCM(env, nodeId, title, body) {
  let accessToken;
  try {
    accessToken = await getAccessToken(env.FIREBASE_SA_JSON);
  } catch (e) {
    return new Response("Failed to get access token: " + e.message, { status: 500 });
  }
  const projectId = env.FIREBASE_PROJECT_ID.replace(/﻿/g, "").trim();
  const topic = "kragwag_" + nodeId.replace(/﻿/g, "").trim();
  const fcmUrl = `https://fcm.googleapis.com/v1/projects/${projectId}/messages:send`;
  const fcmPayload = {
    message: {
      topic,
      notification: { title, body },
      android: {
        priority: "HIGH",
        notification: { sound: "default", channel_id: "kragwag_alerts" }
      }
    }
  };
  const fcmRes = await fetch(fcmUrl, {
    method: "POST",
    headers: { "Authorization": `Bearer ${accessToken}`, "Content-Type": "application/json" },
    body: JSON.stringify(fcmPayload)
  });
  let fcmText = "(no body)";
  try {
    fcmText = await fcmRes.text();
  } catch (_) {
  }
  if (!fcmRes.ok) return new Response("FCM error " + fcmRes.status + ": " + fcmText, { status: 502 });
  return new Response("ok", { status: 200 });
}
__name(sendFCM, "sendFCM");
async function getAccessToken(saJsonString) {
  const sa = JSON.parse(saJsonString);
  const now = Math.floor(Date.now() / 1e3);
  const claim = {
    iss: sa.client_email,
    scope: "https://www.googleapis.com/auth/firebase.messaging",
    aud: "https://oauth2.googleapis.com/token",
    iat: now,
    exp: now + 3600
  };
  const header = b64url(JSON.stringify({ alg: "RS256", typ: "JWT" }));
  const payload = b64url(JSON.stringify(claim));
  const sigInput = `${header}.${payload}`;
  const keyData = pemToArrayBuffer(sa.private_key);
  const cryptoKey = await crypto.subtle.importKey(
    "pkcs8",
    keyData,
    { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" },
    false,
    ["sign"]
  );
  const signature = await crypto.subtle.sign("RSASSA-PKCS1-v1_5", cryptoKey, new TextEncoder().encode(sigInput));
  const jwt = `${sigInput}.${arrayBufferToBase64Url(signature)}`;
  const tokenRes = await fetch("https://oauth2.googleapis.com/token", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: `grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=${jwt}`
  });
  const tokenData = await tokenRes.json();
  if (!tokenData.access_token) throw new Error(JSON.stringify(tokenData));
  return tokenData.access_token;
}
__name(getAccessToken, "getAccessToken");
function b64url(str) {
  return btoa(str).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
__name(b64url, "b64url");
function arrayBufferToBase64Url(buf) {
  const bytes = new Uint8Array(buf);
  let bin = "";
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
__name(arrayBufferToBase64Url, "arrayBufferToBase64Url");
function pemToArrayBuffer(pem) {
  const b64 = pem.replace(/-----BEGIN PRIVATE KEY-----/, "").replace(/-----END PRIVATE KEY-----/, "").replace(/\s/g, "");
  const bin = atob(b64);
  const buf = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
  return buf.buffer;
}
__name(pemToArrayBuffer, "pemToArrayBuffer");
export {
  index_default as default
};
//# sourceMappingURL=index.js.map

--ea7fa4d837bedb4581dde739c114100b94d224ee7d480814edd2da3ab808--
