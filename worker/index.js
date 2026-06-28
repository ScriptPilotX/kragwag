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
      // Store "1" with 15-min TTL — key existence is the freshness signal, not the value.
      // Date.now() in Cloudflare Worker isolates can be stale (cached from isolate creation),
      // so timestamp-based age checks are unreliable. TTL expiry is always accurate.
      await env.HEARTBEAT.put(`hb:${nodeId}`, "1", { expirationTtl: 900 });
      await env.HEARTBEAT.delete(`offline:${nodeId}`);
      await env.HEARTBEAT.put(`reg:${nodeId}`, "1");
      if (body.freeHeap !== undefined)
        await env.HEARTBEAT.put(`heap:${nodeId}`, String(body.freeHeap));
      return new Response("ok", { status: 200 });
    }
    if (url.pathname === "/notify") {
      if (!body.nodeId || !body.title || !body.body)
        return new Response("Missing fields: nodeId, title, body", { status: 400 });
      return await sendFCM(env, body.nodeId, body.title, body.body);
    }
    return new Response("Not found", { status: 404 });
  },
  // Cron: runs every 5 minutes. Checks reg: keys (permanent) so devices offline >15 min
  // are detected even after hb: key expires. Uses key existence, not timestamp age —
  // Date.now() in Workers isolates is unreliable for age calculations.
  async scheduled(event, env, ctx) {
    const list = await env.HEARTBEAT.list({ prefix: "reg:" });
    for (const key of list.keys) {
      const nodeId = key.name.slice(4);
      const hbVal = await env.HEARTBEAT.get(`hb:${nodeId}`);
      const isOffline = !hbVal;  // key expired (>15 min no heartbeat) = offline
      if (isOffline) {
        const alerted = await env.HEARTBEAT.get(`offline:${nodeId}`);
        if (!alerted) {
          await sendFCM(env, nodeId, "KragWag Offline", "The fence monitor has not been seen for over 15 minutes.");
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
