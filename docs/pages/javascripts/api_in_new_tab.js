// The API website has a menu of its own and not the documentation's, so a link to it opens in a
// new tab and the page the reader came from stays open.
(function () {
  "use strict";

  if (document.currentScript === null) {
    throw new Error("api_in_new_tab.js must be loaded by a plain script tag.");
  }
  // The websites work under any address, so the API's folder is found from this script's own.
  const apiFolderAddress = new URL("../api/", document.currentScript.src).href;

  // The link gets its target at the click, in the capture phase: Zensical replaces a page's links
  // when the reader moves to another page, and its own click handler, which runs later, leaves a
  // link that has a target to the browser.
  document.addEventListener(
    "click",
    function (clickEvent) {
      if (!(clickEvent.target instanceof Element)) {
        return;
      }
      const link = clickEvent.target.closest("a");
      if (!(link instanceof HTMLAnchorElement)) {
        return;
      }
      if (link.target !== "" || !link.href.startsWith(apiFolderAddress)) {
        return;
      }
      link.target = "_blank";
      link.relList.add("noopener");
    },
    { capture: true }
  );
})();
