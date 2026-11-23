// Shared by credential fields in the desktop WebViews. Pasting stays enabled.
const blockExport = event => event.preventDefault();

export const secretInputProps = {
  type: "password",
  autoComplete: "new-password",
  spellCheck: false,
  autoCapitalize: "none",
  autoCorrect: "off",
  onCopy: blockExport,
  onCut: blockExport,
  onDragStart: blockExport,
};
