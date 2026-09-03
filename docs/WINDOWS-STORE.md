# Word42 in the Microsoft Store

The Store takes desktop programs in two shapes: an MSIX package, or an
MSI/EXE installer left as it is. Word42 goes as MSIX, because the Store
signs an MSIX itself once it has passed certification — no code-signing
certificate to buy — and because Windows then installs and removes it
cleanly, with no registry keys or uninstall entries of the program's own.

The Inno Setup installer in `build-aux/word42.iss` stays as it is: it is
the download from word42.org, and the two installs sit side by side
without noticing each other.

## Building the package

From an MSYS2 **MINGW64** shell, with the build already done
(`docs/BUILD.md`):

```sh
bash build-aux/bundle-windows.sh builddir dist
bash build-aux/pack-msix.sh builddir dist msix
```

The result is `msix/word42-<version>-win64.msix`, unsigned, about 36 MB.

`pack-msix.sh` does three things. It stages the bundle
`bundle-windows.sh` made. It renders the Store's tiles from
`data/icons/scalable/apps/org.word42.word42.svg` with `rsvg-convert` —
StoreLogo, Square44x44, Square71x71, Square150x150, Square310x310 and
Wide310x150, each at scale 100, 125, 150, 200 and 400, plus the
target-size 16/24/32/48/256 variants of the 44x44 one, plated and
unplated, which are what the taskbar and the Start list actually draw.
And it fills in `data/msix/AppxManifest.xml.in` and packs the lot with
`makeappx.exe` from the Windows 10/11 SDK, found under
`C:\Program Files (x86)\Windows Kits\10\bin\*\x64\`.

Two things about that are worth knowing, because both cost an afternoon
once:

- The scale variants are indexed into `resources.pri` by `makepri.exe`.
  It is run over a directory holding nothing but `Assets` and the
  manifest — pointed at the whole bundle it reads every filename in the
  GTK icon theme as a resource qualifier — and the `<packaging>` block
  its own `createconfig` writes is stripped first, because that block
  splits the scales into separate resource packs and this is one package.
- The SDK tools are native Windows programs. Run them from the MSYS2
  MINGW64 shell, not from a shell whose `bash` comes from a different
  MSYS runtime (Git for Windows', say, with MSYS2 ahead of it on `PATH`):
  a child process across two runtimes gets `ERROR_ACCESS_DENIED` from
  `makepri` with nothing printed to say why.

Requirements the manifest is built around
([package requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements)):

- The version is four numbers, `Major.Minor.Build.Revision`, and the
  Store keeps the fourth for itself: it must be 0 on submission. The
  script maps meson's version to that, dropping any suffix on the way:
  `1.0.1-dev` becomes `1.0.1.0`. The suffix stays in the file's name,
  so a development build is still telling you what it is.
- `Identity/Name` and `Identity/Publisher` have to be exactly what
  Partner Center assigns. See below.
- A full-trust desktop program declares
  `TargetDeviceFamily Name="Windows.Desktop"` with
  `MinVersion 10.0.17763.0` — Windows 10 1809, the floor for MSIX — and
  the `runFullTrust` restricted capability.

## What the manifest declares

- **`runFullTrust`.** Word42 is an ordinary Win32 process tree, not a
  sandboxed app.
- **File types**, through `windows.fileTypeAssociation`: `.doc` and
  `.docx`, `.rtf`, `.odt`, `.abw` and `.zabw`, `.txt` and `.text`. This
  puts Word42 in the Open with list and in Settings ▸ Default apps; it
  takes nothing from whatever holds the association now. Word42 also
  reads `.pdf`, `.htm`, `.html` and `.pptx`, which are deliberately left
  out: a word processor has no business appearing in the list of things
  that open a web page.
- **`windows.appExecutionAlias`**, so that `word42` works in a console.
  A packaged program has no fixed install path to put on `PATH`.
- **One `en-US` resource.** The interface is in English; nothing is
  translated through Windows resources, so declaring more languages would
  only add listings to keep up to date.

## Partner Center identity

The name **Word42** is reserved, so the identity exists and
`pack-msix.sh` has it built in — a plain run makes the package the Store
expects, with nothing to pass:

| Manifest | Value |
|---|---|
| `Package/Identity/Name` | `29567TheFreecivProject.Word42` |
| `Package/Identity/Publisher` | `CN=631F98F7-2280-49EE-8EF8-534CC36D09CF` |
| `Package/Properties/PublisherDisplayName` | `Nordstjernen` |

None of the three is ours to choose. The Package Family Name is derived
from the name and the publisher, so a package that renames either is
refused on upload. The prefix `29567TheFreecivProject` and the publisher
display name `Nordstjernen` belong to the Partner Center account rather
than to this product, and they stay as they are; the name a customer sees
is the `DisplayName`, which is **Word42**.

Calculated from those, and worth checking a package against:

- Package Family Name `29567TheFreecivProject.Word42_ga6t65cntcpba`
- Store ID `9NKG7V7CRX06`
- <https://apps.microsoft.com/detail/9NKG7V7CRX06>, and
  `ms-windows-store://pdp/?productid=9NKG7V7CRX06`

That last suffix is a hash of the publisher string, which makes it a
free check that the identity is right: register the staged layout (below)
and, even when the install itself is refused, Windows names the package
it was about to make. `..._x64__ga6t65cntcpba` means the publisher
matches to the byte.

`W42_MSIX_IDENTITY_NAME`, `W42_MSIX_PUBLISHER` and
`W42_MSIX_PUBLISHER_DISPLAY` override the three for a different product
or a different account.

## Trying it on this machine

The package is submitted unsigned, and Windows will not install an
unsigned MSIX. Two ways round that:

1. **Register the staged layout.** Needs Developer Mode on in Windows
   Settings, and no signing at all:

   ```powershell
   Add-AppxPackage -Register msix\stage\AppxManifest.xml
   ```

2. **Sign it with a test certificate**, whose subject must equal the
   manifest's `Publisher` exactly:

   ```powershell
   New-SelfSignedCertificate -Type Custom -CertStoreLocation Cert:\CurrentUser\My `
     -Subject 'CN=631F98F7-2280-49EE-8EF8-534CC36D09CF' `
     -KeyUsage DigitalSignature -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
   # export it to a .pfx, trust it in the machine's Trusted People store, then
   #   W42_MSIX_CERT_PFX=/c/path/test.pfx bash build-aux/pack-msix.sh builddir dist msix
   Add-AppxPackage msix\word42-<version>-win64.msix
   ```

Then check that Word42 starts from the Start menu, that a `.rtf` offers
it under Open with, that `word42` runs in a fresh terminal, and that
removing it from the Start menu leaves nothing behind.

What is different about running packaged: the program lives under
`C:\Program Files\WindowsApps\<identity>\`, which is read-only and
system-managed, and its writes to `%LOCALAPPDATA%` are redirected into
`%LOCALAPPDATA%\Packages\<identity>\LocalCache\Local\`. Settings,
autosaved documents and recovery files all keep working; they are just
somewhere else, which matters when asking someone where their recovered
document went.

## Before submitting

| Policy | What it wants | Word42 |
|---|---|---|
| 10.1 | An accurate listing, screenshots, a title that is the program's own | Copy below; screenshots from `docs/images/` |
| 10.2.2/3 | No malware, nothing installed on the side | One self-contained bundle; it downloads nothing |
| 10.2.7 | A clean uninstall | Managed by Windows |
| 10.5.1 | A privacy policy URL — **required for every full-trust program, whether or not it collects anything** | **To do:** publish one at word42.org/privacy and paste the URL in |
| 10.8 | Commerce | Free, nothing to buy |
| 11.x | Content | The IARC questionnaire during submission; a word processor rates for everyone |

The restricted capability needs a sentence of justification in the
submission:

```text
Capability requested: runFullTrust

Word42 is a packaged Win32 desktop word processor. The capability is
needed only to run the bundled desktop executable from the MSIX package.
The application does not request elevation, install services or drivers,
change system settings, manage other packages, or collect telemetry.
```

Check the [Store policies](https://learn.microsoft.com/en-us/windows/apps/publish/store-policies)
again before submitting — the version number moves several times a year.

## The submission itself

The account exists and the name **Word42** is reserved; what is left is
the submission itself.

1. Build the package — `bash build-aux/pack-msix.sh builddir dist msix`
   — and upload `msix/word42-<version>-win64.msix`. **Do not sign it**:
   the Store signs it after certification, and an already-signed package
   is refused.
2. Properties: category *Productivity*; Windows 10 1809 or later, x64.
3. Age rating: the IARC questionnaire.
4. Privacy policy URL. The form will not take a full-trust program
   without one, and there is nothing at word42.org/privacy yet — this is
   the one thing still missing.
5. The listing, en-US at least; the copy below is ready to paste.
6. Certification takes roughly one to three days.

## The listing, ready to paste

**Display name:** Word42

**Short description** (≤100 characters):

> A word processor in the shape of the ones that taught people to write
> on a screen.

**Description:**

> Word42 is a word processor that works the way the ones from the early
> nineties worked: a menu bar, a toolbar, a ruler you can drag, and a
> page on the screen that is the page that comes out of the printer.
> Nothing appears when you did not ask for it, and nothing is hidden
> three panels deep.
>
> - **It opens what you already have.** Word documents old and new, Rich
>   Text, OpenDocument, AbiWord, plain text, web pages, and PDF; it
>   writes all of those, and PDF besides.
> - **Everything a document needs.** Styles, tables, footnotes and
>   endnotes, headers and footers, sections and columns, a table of
>   contents, mail merge, spelling in several languages, hyphenation,
>   revision marks and comments.
> - **Yours alone.** No account, no cloud, no telemetry. Your documents
>   are files on your disk, in formats other programs can read.
> - **Free software**, under the GNU General Public License.

**Product features:**

- Menus, toolbar and a live ruler — the interface of a word processor
- Reads and writes Word, RTF, OpenDocument, AbiWord, HTML and text
- Exports PDF, and reads it too
- Styles, tables, footnotes, sections, columns, a table of contents
- Spelling and hyphenation, revision marks and comments
- No account, no cloud, no telemetry
- Free software under the GPL

**Search terms:** `word processor`, `document editor`, `rtf`, `doc`,
`odt`, `open source`, `classic`

**Category:** Productivity. **Price:** free.

## Sources

- [MSIX app package requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements)
- [Distributing a Win32 app through the Store](https://learn.microsoft.com/en-us/windows/apps/distribute-through-store/how-to-distribute-your-win32-app-through-microsoft-store)
- [Microsoft Store policies](https://learn.microsoft.com/en-us/windows/apps/publish/store-policies)
