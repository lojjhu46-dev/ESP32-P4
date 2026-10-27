(function () {
  "use strict";

  var state = { tab: "events", offset: 0, limit: 100, lastLen: 0, timer: null, autoTo: true };

  function $(id) { return document.getElementById(id); }

  function fmtTime(ts) {
    if (!ts) return "";
    return new Date(ts * 1000).toLocaleString("zh-CN", {
      timeZone: "Asia/Shanghai", hour12: false
    });
  }

  function pad(n) { return n < 10 ? "0" + n : String(n); }

  function localInput(date) {
    return date.getFullYear() + "-" + pad(date.getMonth() + 1) + "-" + pad(date.getDate()) +
      "T" + pad(date.getHours()) + ":" + pad(date.getMinutes());
  }

  function notify(el, text, isErr) {
    var node = $(el);
    if (!node) return;
    node.textContent = text || "";
    node.style.color = isErr ? "var(--danger)" : "";
  }

  function showLogin() {
    if (state.timer) { clearInterval(state.timer); state.timer = null; }
    $("app").classList.add("hidden");
    $("login").classList.remove("hidden");
    $("loginToken").focus();
  }

  function showApp() {
    $("login").classList.add("hidden");
    $("app").classList.remove("hidden");
  }

  function api(path, opts) {
    var init = Object.assign({ credentials: "same-origin" }, opts || {});
    return fetch(path, init).then(function (r) {
      var ct = r.headers.get("content-type") || "";
      var data = ct.indexOf("application/json") >= 0 ? r.json() : Promise.resolve(null);
      return data.then(function (body) {
        if (r.status === 401) { showLogin(); throw new Error("未登录"); }
        if (!r.ok) { throw new Error((body && body.error) || ("HTTP " + r.status)); }
        return body;
      });
    });
  }

  /* ---------------- login ---------------- */
  $("loginForm").addEventListener("submit", function (e) {
    e.preventDefault();
    var token = $("loginToken").value.trim();
    if (!token) { notify("loginErr", "请输入 Token", true); return; }
    api("/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ token: token })
    }).then(function () {
      $("loginToken").value = "";
      notify("loginErr", "");
      showApp();
      switchTab("events");
    }).catch(function (err) {
      notify("loginErr", err.message, true);
    });
  });

  $("logout").addEventListener("click", function () {
    api("/api/logout", { method: "POST" }).catch(function () { }).then(showLogin);
  });

  /* ---------------- tabs ---------------- */
  function switchTab(tab) {
    state.tab = tab;
    Array.prototype.forEach.call(document.querySelectorAll("#tabs button"), function (b) {
      b.classList.toggle("active", b.getAttribute("data-tab") === tab);
    });
    ["events", "persons", "bank", "devices"].forEach(function (name) {
      $("tab-" + name).classList.toggle("hidden", name !== tab);
    });
    if (tab === "events") loadEvents(true);
    if (tab === "persons") loadPersons();
    if (tab === "bank") loadBank();
    if (tab === "devices") loadDevices();
  }

  Array.prototype.forEach.call(document.querySelectorAll("#tabs button"), function (b) {
    b.addEventListener("click", function () { switchTab(b.getAttribute("data-tab")); });
  });

  /* ---------------- events ---------------- */
  function filterParams(withRange) {
    var p = new URLSearchParams();
    if (withRange) {
      var from = $("from").value;
      var to = $("to").value;
      if (from) p.set("from", String(Math.floor(new Date(from).getTime() / 1000)));
      /* datetime-local 只有分钟精度：+60s 才不会漏掉当前这一分钟刚签到的记录 */
      if (to) p.set("to", String(Math.floor(new Date(to).getTime() / 1000) + 60));
    }
    var person = $("personFilter").value.trim();
    var device = $("deviceFilter").value.trim();
    if (person) p.set("person", person);
    if (device) p.set("device", device);
    return p;
  }

  function addCell(tr, text, cls) {
    var td = document.createElement("td");
    td.textContent = text == null ? "" : String(text);
    if (cls) td.className = cls;
    tr.appendChild(td);
    return td;
  }

  function loadEvents(reset) {
    if (reset) state.offset = 0;
    if (state.autoTo) $("to").value = localInput(new Date(Date.now() + 60000));
    var p = filterParams(true);
    p.set("limit", String(state.limit));
    p.set("offset", String(state.offset));
    notify("eventMeta", "查询中…");
    api("/api/events?" + p.toString()).then(function (data) {
      var tbody = $("eventTable").querySelector("tbody");
      tbody.innerHTML = "";
      var events = data.events || [];
      state.lastLen = events.length;
      events.forEach(function (ev) {
        var tr = document.createElement("tr");
        addCell(tr, fmtTime(ev.ts));
        var nameTd = addCell(tr, ev.person);
        if (ev.unknown) {
          var tag = document.createElement("span");
          tag.className = "tag";
          tag.textContent = "未入库";
          nameTd.appendChild(tag);
        }
        addCell(tr, ev.sim == null ? "" : Number(ev.sim).toFixed(3));
        addCell(tr, ev.device);
        addCell(tr, ev.link || "");
        var photoTd = document.createElement("td");
        if (ev.photo_url) {
          var btn = document.createElement("button");
          btn.textContent = "查看";
          btn.addEventListener("click", function () { openPhoto(ev); });
          photoTd.appendChild(btn);
        } else {
          photoTd.textContent = "—";
          photoTd.className = "muted";
        }
        tr.appendChild(photoTd);
        tbody.appendChild(tr);
      });
      if (!events.length) {
        var tr = document.createElement("tr");
        var td = document.createElement("td");
        td.colSpan = 6;
        td.className = "muted";
        td.textContent = "没有符合条件的记录";
        tr.appendChild(td);
        tbody.appendChild(tr);
      }
      var page = Math.floor(state.offset / state.limit) + 1;
      notify("eventMeta", "第 " + page + " 页 · " + events.length + " 条");
      $("pageInfo").textContent = "第 " + page + " 页";
      $("prev").disabled = state.offset <= 0;
      $("next").disabled = events.length < state.limit;
    }).catch(function (err) {
      notify("eventMeta", err.message, true);
    });
  }

  $("to").addEventListener("change", function () { state.autoTo = false; });
  $("search").addEventListener("click", function () { loadEvents(true); });
  $("personFilter").addEventListener("keydown", function (e) { if (e.key === "Enter") loadEvents(true); });
  $("deviceFilter").addEventListener("keydown", function (e) { if (e.key === "Enter") loadEvents(true); });
  $("prev").addEventListener("click", function () {
    state.offset = Math.max(0, state.offset - state.limit);
    loadEvents(false);
  });
  $("next").addEventListener("click", function () {
    if (state.lastLen < state.limit) return;
    state.offset += state.limit;
    loadEvents(false);
  });
  $("csv").addEventListener("click", function () {
    var p = filterParams(true);
    window.location.href = "/api/events.csv?" + p.toString();
  });

  /* ---------------- persons ---------------- */
  function loadPersons() {
    notify("personsMeta", "加载中…");
    api("/api/persons").then(function (data) {
      var tbody = $("personTable").querySelector("tbody");
      tbody.innerHTML = "";
      (data.persons || []).forEach(function (p) {
        var tr = document.createElement("tr");
        addCell(tr, p.name);
        addCell(tr, p.feats);
        var st = document.createElement("td");
        st.textContent = p.active ? "在册" : "已移除";
        if (!p.active) st.className = "muted";
        tr.appendChild(st);
        addCell(tr, p.sign_count);
        addCell(tr, p.last_sign ? fmtTime(p.last_sign) : "—");
        tbody.appendChild(tr);
      });
      if (!(data.persons || []).length) {
        var tr = document.createElement("tr");
        var td = document.createElement("td");
        td.colSpan = 5; td.className = "muted"; td.textContent = "云端还没有人员，请先在板端注册并同步人脸库";
        tr.appendChild(td); tbody.appendChild(tr);
      }
      notify("personsMeta", "共 " + (data.persons || []).length + " 人");
    }).catch(function (err) { notify("personsMeta", err.message, true); });
  }
  $("reloadPersons").addEventListener("click", loadPersons);

  /* ---------------- face bank ---------------- */
  function loadBank() {
    notify("bankMeta", "加载中…");
    api("/api/facebank/meta").then(function (data) {
      var tbody = $("bankTable").querySelector("tbody");
      tbody.innerHTML = "";
      var list = data.snapshots || [];
      list.forEach(function (s) {
        var tr = document.createElement("tr");
        addCell(tr, s.id);
        addCell(tr, fmtTime(s.ts));
        addCell(tr, s.device);
        addCell(tr, (s.size / 1024).toFixed(1) + " KB");
        addCell(tr, ("00000000" + (s.crc32 >>> 0).toString(16)).slice(-8));
        addCell(tr, s.persons);
        addCell(tr, s.feats);
        var td = document.createElement("td");
        var a = document.createElement("a");
        a.className = "button";
        a.href = "/api/facebank/latest?id=" + s.id;
        a.textContent = "下载";
        td.appendChild(a);
        tr.appendChild(td);
        tbody.appendChild(tr);
      });
      if (!list.length) {
        var tr = document.createElement("tr");
        var td = document.createElement("td");
        td.colSpan = 8; td.className = "muted"; td.textContent = "还没有人脸库快照";
        tr.appendChild(td); tbody.appendChild(tr);
      }
      notify("bankMeta", "共 " + list.length + " 个版本");
    }).catch(function (err) { notify("bankMeta", err.message, true); });
  }
  $("reloadBank").addEventListener("click", loadBank);

  $("uploadBank").addEventListener("change", function () {
    var file = this.files && this.files[0];
    if (!file) return;
    if (file.size > 4 * 1024 * 1024) { notify("bankMeta", "文件超过 4MB", true); return; }
    notify("bankMeta", "上传中…");
    var reader = new FileReader();
    reader.onload = function () {
      fetch("/api/facebank", {
        method: "POST",
        credentials: "same-origin",
        headers: { "Content-Type": "application/octet-stream", "X-Source": "web" },
        body: reader.result
      }).then(function (r) {
        return r.json().catch(function () { return {}; }).then(function (body) {
          if (!r.ok) throw new Error(body.error || ("HTTP " + r.status));
          notify("bankMeta", "上传成功：版本 #" + body.id + " · " + body.persons + " 人 / " + body.feats + " 特征");
          loadBank();
        });
      }).catch(function (err) { notify("bankMeta", err.message, true); });
    };
    reader.onerror = function () { notify("bankMeta", "读取文件失败", true); };
    reader.readAsArrayBuffer(file);
    this.value = "";
  });

  /* ---------------- devices ---------------- */
  function loadDevices() {
    notify("devicesMeta", "加载中…");
    api("/api/devices").then(function (data) {
      var tbody = $("deviceTable").querySelector("tbody");
      tbody.innerHTML = "";
      (data.devices || []).forEach(function (d) {
        var tr = document.createElement("tr");
        addCell(tr, d.id);
        addCell(tr, d.last_seen ? fmtTime(d.last_seen) : "—");
        addCell(tr, d.lib_n == null ? "—" : d.lib_n);
        addCell(tr, d.fw || "—");
        tbody.appendChild(tr);
      });
      if (!(data.devices || []).length) {
        var tr = document.createElement("tr");
        var td = document.createElement("td");
        td.colSpan = 4; td.className = "muted"; td.textContent = "还没有设备心跳";
        tr.appendChild(td); tbody.appendChild(tr);
      }
      notify("devicesMeta", "共 " + (data.devices || []).length + " 台");
    }).catch(function (err) { notify("devicesMeta", err.message, true); });
  }
  $("reloadDevices").addEventListener("click", loadDevices);

  /* ---------------- photo modal ---------------- */
  function openPhoto(ev) {
    $("photoImg").src = ev.photo_url + "?t=" + Date.now();
    $("photoCaption").textContent = fmtTime(ev.ts) + " · " + ev.person +
      (ev.sim == null ? "" : " · " + Number(ev.sim).toFixed(3));
    $("photoModal").classList.remove("hidden");
  }
  function closePhoto() {
    $("photoModal").classList.add("hidden");
    $("photoImg").removeAttribute("src");
  }
  $("closePhoto").addEventListener("click", closePhoto);
  $("photoModal").addEventListener("click", function (e) {
    if (e.target === this) closePhoto();
  });
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") closePhoto();
  });

  /* ---------------- boot ---------------- */
  var now = new Date();
  var dayStart = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 0, 0);
  $("from").value = localInput(dayStart);
  $("to").value = localInput(new Date(now.getTime() + 60000));

  api("/api/persons").then(function () {
    showApp();
    switchTab("events");
    state.timer = setInterval(function () {
      if (state.tab === "events" && document.visibilityState === "visible") {
        loadEvents(false);
      }
    }, 15000);
  }).catch(function (err) {
    if (err.message !== "未登录") {
      showLogin();
      notify("loginErr", "无法连接服务：" + err.message, true);
    }
  });
})();
