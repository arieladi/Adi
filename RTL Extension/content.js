// RTL/LTR Auto Direction Extension
// Created by Adi Ariel using Claude
// Automatically detects Hebrew/Arabic and switches text direction

function isRTLChar(char) {
  const code = char.codePointAt(0);
  return (
    (code >= 0x0590 && code <= 0x05FF) ||   // Hebrew
    (code >= 0xFB1D && code <= 0xFB4F) ||   // Hebrew presentation forms
    (code >= 0x0600 && code <= 0x06FF) ||   // Arabic
    (code >= 0x0750 && code <= 0x077F) ||   // Arabic supplement
    (code >= 0xFB50 && code <= 0xFDFF) ||   // Arabic presentation forms A
    (code >= 0xFE70 && code <= 0xFEFF)      // Arabic presentation forms B
  );
}

function isLTRChar(char) {
  const code = char.codePointAt(0);
  return (
    (code >= 0x0041 && code <= 0x005A) ||   // A-Z
    (code >= 0x0061 && code <= 0x007A)      // a-z
  );
}

function setDirection(el, dir) {
  el.style.direction = dir;
  el.style.textAlign = dir === 'rtl' ? 'right' : 'left';
  el.dir = dir;
}

function getCurrentLine(el) {
  const text = el.value !== undefined ? el.value : el.innerText;
  const cursorPos = el.selectionStart !== undefined ? el.selectionStart : null;

  if (cursorPos !== null) {
    const textUpToCursor = text.substring(0, cursorPos);
    const lines = textUpToCursor.split('\n');
    return lines[lines.length - 1];
  } else {
    const selection = window.getSelection();
    if (!selection || selection.rangeCount === 0) return '';
    const range = selection.getRangeAt(0);
    const node = range.startContainer;
    const lineText = node.textContent || '';
    const offset = range.startOffset;
    const textUpToCursor = lineText.substring(0, offset);
    const lines = textUpToCursor.split('\n');
    return lines[lines.length - 1];
  }
}

// Manual override shortcuts
document.addEventListener('keydown', function(e) {
  const el = document.activeElement;
  const isEditable = el && (
    el.tagName === 'INPUT' ||
    el.tagName === 'TEXTAREA' ||
    el.isContentEditable
  );
  if (!isEditable) return;

  // Right Ctrl + Right Shift → force RTL
  if (e.code === 'ShiftRight' && e.ctrlKey) { e.preventDefault(); setDirection(el, 'rtl'); return; }
  // Left Ctrl + Left Shift → force LTR
  if (e.code === 'ShiftLeft' && e.ctrlKey) { e.preventDefault(); setDirection(el, 'ltr'); return; }
}, true);

// Auto-detect on first character of each line only
document.addEventListener('input', function(e) {
  const el = e.target;
  const isEditable = el && (
    el.tagName === 'INPUT' ||
    el.tagName === 'TEXTAREA' ||
    el.isContentEditable
  );
  if (!isEditable) return;

  const currentLine = getCurrentLine(el);
  const trimmed = currentLine.trim();

  // Only trigger on the very first character of a line
  if (trimmed.length !== 1) return;

  const firstChar = trimmed[0];
  const currentDir = el.style.direction || window.getComputedStyle(el).direction || 'ltr';

  if (isRTLChar(firstChar) && currentDir !== 'rtl') {
    setDirection(el, 'rtl');
  } else if (isLTRChar(firstChar) && currentDir !== 'ltr') {
    setDirection(el, 'ltr');
  }
}, true);

// Auto-detect direction when clicking into a text box that already has text
document.addEventListener('focusin', function(e) {
  const el = e.target;
  const isEditable = el && (
    el.tagName === 'INPUT' ||
    el.tagName === 'TEXTAREA' ||
    el.isContentEditable
  );
  if (!isEditable) return;
  const text = el.value !== undefined ? el.value : el.innerText;
  if (!text || text.trim() === '') return;
  for (let i = 0; i < text.length; i++) {
    const char = text[i];
    if (isRTLChar(char)) { setDirection(el, 'rtl'); return; }
    if (isLTRChar(char)) { setDirection(el, 'ltr'); return; }
  }
});

let pendingEnglishChar = null;


let blockMacGhostChar = false;

// Step 1: Detect the physical key, inject the English letter, and arm the Ghost Catcher
document.addEventListener('keydown', function(event) {
    if (event.shiftKey && !event.metaKey && !event.ctrlKey && !event.altKey) {
        
        const target = event.target;
        const isTextInput = target.tagName === 'INPUT' || 
                            target.tagName === 'TEXTAREA' || 
                            target.isContentEditable;
        
        if (!isTextInput) return;

        // If it's already an English capital letter, let the OS handle it naturally
        if (/^[A-Z]$/.test(event.key)) return;

        const englishMap = {
            'KeyA': 'A', 'KeyB': 'B', 'KeyC': 'C', 'KeyD': 'D', 'KeyE': 'E',
            'KeyF': 'F', 'KeyG': 'G', 'KeyH': 'H', 'KeyI': 'I', 'KeyJ': 'J',
            'KeyK': 'K', 'KeyL': 'L', 'KeyM': 'M', 'KeyN': 'N', 'KeyO': 'O',
            'KeyP': 'P', 'KeyQ': 'Q', 'KeyR': 'R', 'KeyS': 'S', 'KeyT': 'T',
            'KeyU': 'U', 'KeyV': 'V', 'KeyW': 'W', 'KeyX': 'X', 'KeyY': 'Y',
            'KeyZ': 'Z'
        };

        const charToInsert = englishMap[event.code];

        if (charToInsert) {
            // Stop the default Mac keydown behavior (which types Nikkud or Hebrew)
            event.preventDefault();
            event.stopImmediatePropagation();
            
            // Arm the ghost catcher for the next 50 milliseconds
            blockMacGhostChar = true;
            setTimeout(() => { blockMacGhostChar = false; }, 50);

            // Inject our English letter instantly
            if (target.isContentEditable) {
                document.execCommand('insertText', false, charToInsert);
            } else {
                const start = target.selectionStart;
                const end = target.selectionEnd;
                const val = target.value;
                target.value = val.slice(0, start) + charToInsert + val.slice(end);
                target.selectionStart = target.selectionEnd = start + 1;
                target.dispatchEvent(new Event('input', { bubbles: true }));
            }
        }
    }
}, true);

// Step 2: The Ghost Catcher
// This intercepts the Hebrew/Nikkud character the Mac forces into the text box milliseconds later
document.addEventListener('beforeinput', function(event) {
    // If the catcher is armed and there is data trying to be written
    if (blockMacGhostChar && event.data) {
        // Check if the Mac is trying to insert a Hebrew character or Nikkud (\u0590-\u05FF)
        if (/[\u0590-\u05FF]/.test(event.data)) {
            // Block it completely!
            event.preventDefault();
            event.stopImmediatePropagation();
        }
    }
}, true);