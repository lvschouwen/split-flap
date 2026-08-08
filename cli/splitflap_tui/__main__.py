from .app import SplitflapApp
from .config import load_config


def main() -> None:
    SplitflapApp(load_config()).run()


if __name__ == "__main__":
    main()
