# Install the signed universal executable and its license information.
class Gd < Formula
  desc "Command-line runtime for typed scripts, services, and data workflows"
  homepage "https://gd.progsha.com/"
  url "https://github.com/prog-sha/gd/releases/download/0.7.4-stable/gd-macos-universal.zip"
  version "0.7.4"
  sha256 "f9524b7262cbee625d3658e541b933d017e0d89b89bb9d56fc9f2cb1a8511407"
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
