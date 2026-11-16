"""Macros that Zensical makes available to the pages under docs/pages/."""

from functools import cache
from pathlib import Path
from xml.etree import ElementTree

API_TAG_FILE = Path(__file__).resolve().parent.parent / "build" / "falcon_api.tag"
API_FOLDER_IN_PAGES = "api"
LINKABLE_KINDS = frozenset({"class", "struct", "concept"})


@cache
def read_api_page_by_name(tag_file: Path) -> dict[str, str]:
    """Map each class, struct and concept in Doxygen's tag file to its page's file name."""
    if not tag_file.is_file():
        raise FileNotFoundError(
            f"{tag_file} is missing: build the API with Doxygen before the documentation."
        )
    api_page_by_name: dict[str, str] = {}
    for compound in ElementTree.parse(tag_file).getroot().iterfind("compound"):
        if compound.get("kind") not in LINKABLE_KINDS:
            continue
        name = compound.findtext("name")
        page_file_name = compound.findtext("filename")
        if not name or not page_file_name:
            raise ValueError(f"{tag_file} holds a {compound.get('kind')} without a name or a page.")
        api_page_by_name[name] = page_file_name
    return api_page_by_name


def path_to_pages_folder(page_path: str) -> str:
    """Give the relative path from a page's Markdown file to docs/pages/, e.g. "../" for "a/b.md".

    Zensical resolves a link against the Markdown file, then rewrites it for the page's address.
    """
    return "../" * page_path.count("/")


def define_env(env):
    """Register the macros; Zensical calls this once for every page it renders."""

    @env.macro
    def api(name: str) -> str:
        """Link a class, struct or concept, by its full C++ name, to its page on the API website."""
        api_page_by_name = read_api_page_by_name(API_TAG_FILE)
        if name not in api_page_by_name:
            raise KeyError(
                f'api("{name}"): the API has no class, struct or concept of that name '
                f"(looked in {API_TAG_FILE})."
            )
        if env.page is None:
            raise RuntimeError(f'api("{name}") was called outside a page.')
        address = (
            path_to_pages_folder(env.page.path)
            + API_FOLDER_IN_PAGES
            + "/"
            + api_page_by_name[name]
        )
        return f"[`{name}`]({address})"
