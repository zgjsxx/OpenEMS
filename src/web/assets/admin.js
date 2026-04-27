const OpenEMSAdmin = (() => {
  function escapeHtml(value) {
    return String(value ?? "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#039;");
  }

  function formatDateTime(value) {
    if (!value) return "-";
    const date = value instanceof Date ? value : new Date(value);
    if (Number.isNaN(date.getTime())) return "-";
    const yyyy = date.getFullYear();
    const mm = String(date.getMonth() + 1).padStart(2, "0");
    const dd = String(date.getDate()).padStart(2, "0");
    const hh = String(date.getHours()).padStart(2, "0");
    const mi = String(date.getMinutes()).padStart(2, "0");
    const ss = String(date.getSeconds()).padStart(2, "0");
    return `${yyyy}-${mm}-${dd} ${hh}:${mi}:${ss}`;
  }

  function formatShortDateTime(value) {
    if (!value) return "-";
    const date = value instanceof Date ? value : new Date(value);
    if (Number.isNaN(date.getTime())) return "-";
    const hh = String(date.getHours()).padStart(2, "0");
    const mi = String(date.getMinutes()).padStart(2, "0");
    const ss = String(date.getSeconds()).padStart(2, "0");
    return `${hh}:${mi}:${ss}`;
  }

  function qualityPill(text) {
    const value = String(text || "Unknown");
    const level = value === "Good" ? "success" : value === "Bad" || value === "Invalid" ? "danger" : "warn";
    return `<span class="pill ${level}">${escapeHtml(value)}</span>`;
  }

  function statusPill(text) {
    const value = String(text || "unknown");
    const lower = value.toLowerCase();
    let klass = "muted";
    if (["active", "success", "online", "acked"].includes(lower)) klass = "success";
    else if (["silenced", "pending", "executing", "warning"].includes(lower)) klass = "warn";
    else if (["failed", "disabled", "critical", "cleared"].includes(lower)) klass = "danger";
    else if (["viewer", "operator", "admin"].includes(lower)) klass = "info";
    return `<span class="pill ${klass}">${escapeHtml(value)}</span>`;
  }

  async function api(path, options = {}) {
    const response = await fetch(path, {
      credentials: "same-origin",
      headers: {
        "Content-Type": "application/json",
        ...(options.headers || {}),
      },
      ...options,
    });

    let data = null;
    try {
      data = await response.json();
    } catch (_) {
      data = null;
    }

    if (response.status === 401) {
      window.location.href = "/login";
      throw new Error((data && data.error) || "Unauthorized");
    }
    if (!response.ok) {
      let message = (data && data.error) || `Request failed: ${response.status}`;
      if (data && Array.isArray(data.errors) && data.errors.length) {
        const details = data.errors.slice(0, 5).map((item) => {
          const location = [
            item.table,
            item.row !== null && item.row !== undefined ? `row ${Number(item.row) + 1}` : "",
            item.column || "",
          ].filter(Boolean).join(".");
          return location ? `${location}: ${item.message}` : item.message;
        });
        message = `${message}: ${details.join("; ")}`;
        if (data.errors.length > details.length) {
          message += `; and ${data.errors.length - details.length} more`;
        }
      }
      throw new Error(message);
    }
    return data;
  }

  async function loadMe() {
    return api("/api/auth/me");
  }

  async function bindChrome(activePage) {
    const me = await loadMe();
    const user = me.user || {};
    const usernameEl = document.getElementById("current-username");
    const roleEl = document.getElementById("current-role");
    if (usernameEl) usernameEl.textContent = user.username || "-";
    if (roleEl) roleEl.innerHTML = statusPill(user.role || "-");

    document.querySelectorAll(".nav-link").forEach((link) => {
      if (link.dataset.page === activePage) {
        link.classList.add("active");
      }
    });
    if (user.role !== "admin") {
      document.querySelectorAll(".admin-only").forEach((item) => item.hidden = true);
    }
    const logoutBtn = document.getElementById("logout-btn");
    if (logoutBtn) {
      logoutBtn.onclick = async () => {
        await api("/api/auth/logout", { method: "POST" });
        window.location.href = "/login";
      };
    }
    return me;
  }

  return {
    api,
    bindChrome,
    escapeHtml,
    formatDateTime,
    formatShortDateTime,
    qualityPill,
    statusPill,
  };
})();
