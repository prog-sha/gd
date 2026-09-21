# Install the signed universal executable and its license information.
class Gd < Formula
  desc "Command-line runtime for typed scripts, services, and data workflows"
  homepage "https://gd.progsha.com/"
  url "https://github.com/prog-sha/gd/releases/download/0.7.5-stable/gd-macos-universal.zip"
  version "0.7.5"
  sha256 "cc433bacfa0ac0715e74de35ab6166bf0d0c40a626a2b4fc5186029486649ff7"
  license "MIT"
  depends_on :macos
  skip_clean "bin/gd" # Preserve the executable signature.

  # Install the published executable without rewriting its load commands.
  def install
    bin.install "gd"
    doc.install "LICENSE.txt", "COPYRIGHT.txt", "AUTHORS.md", "NOTICE.md"
  end

  # Confirm startup and script evaluation after installation.
  test do
    assert_match version.to_s, shell_output("#{bin}/gd --version")
    assert_equal "42", shell_output("#{bin}/gd eval 'print(6 * 7)' ").strip
  end
end
