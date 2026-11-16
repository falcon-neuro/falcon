// Zensical draws its search dialog over a blur of the whole page. A browser that draws without a
// graphics card computes that blur again for every frame while the dialog is open, which takes it
// over 100 ms per frame. The dialog sits in a shadow root, out of reach of the page's stylesheets,
// so its rules are added here.
(function () {
  "use strict";

  const dialogStyleSheet = new CSSStyleSheet();
  dialogStyleSheet.replaceSync(`
    * {
      -webkit-backdrop-filter: none !important;
      backdrop-filter: none !important;
    }

    /* Without the blur the page's text would show through the panel, so the panel is opaque. The
       veil over the page is the dialog's only empty child and stays translucent. */
    dialog > :not(:empty) {
      background-color: rgb(var(--color-background)) !important;
    }

    /* On a narrow screen the filter list lies over the results. */
    @media (max-width: 680px) {
      dialog > :not(:empty) > :nth-child(2) {
        background-color: rgb(var(--color-background-subtle)) !important;
      }
    }
  `);

  function adoptDialogStyleSheet(node) {
    if (!(node instanceof Element) || node.shadowRoot === null) {
      return;
    }
    const shadowRoot = node.shadowRoot;
    if (shadowRoot.adoptedStyleSheets.includes(dialogStyleSheet)) {
      return;
    }
    shadowRoot.adoptedStyleSheets = [...shadowRoot.adoptedStyleSheets, dialogStyleSheet];
  }

  for (const child of document.body.children) {
    adoptDialogStyleSheet(child);
  }

  // Zensical may add the dialog's element to the page after this script has run.
  const bodyObserver = new MutationObserver(function (mutations) {
    for (const mutation of mutations) {
      for (const addedNode of mutation.addedNodes) {
        adoptDialogStyleSheet(addedNode);
      }
    }
  });
  bodyObserver.observe(document.body, { childList: true });
})();
