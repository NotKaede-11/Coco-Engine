"""Chess960 oracle checks retained from the locally validated candidate suite."""
from collections import Counter
import re
import random
import chess

def exchange(engine, commands):
    engine.send(commands + "\nisready")
    out = engine.receive("readyok")
    assert not any("rejected" in line or "Illegal move" in line for line in out), out
    return out

def children(board, depth):
    if depth == 1:
        return {move.uci(): 1 for move in board.legal_moves}
    counts = {}
    for move in board.legal_moves:
        board.push(move)
        counts[move.uci()] = board.legal_moves.count() if depth == 2 else sum(children(board, depth - 1).values())
        board.pop()
    return counts

def fen(board):
    return board.fen(shredder=board.chess960, en_passant="xfen")

def divide(engine, board, depth, command=None):
    lines = exchange(engine, (command or ("position fen " + fen(board))) + f"\nd\neval\ngo perft {depth}\nd\neval")
    entries = [line.split(": ") for line in lines if re.fullmatch(r"[a-h][1-8][a-h][1-8][qrbn]?: \d+", line)]
    actual = {move: int(count) for move, count in entries}
    expected = children(board, depth)
    assert actual == expected and len(entries) == len(actual), (fen(board), depth, expected, actual)
    assert f"Total nodes: {sum(expected.values())}" in lines
    assert [line for line in lines if line.startswith("Fen:")] == [f"Fen: {fen(board)}"] * 2
    hashes = [line for line in lines if line.startswith("Zobrist Hash:")]
    evaluations = [line for line in lines if line.startswith("Evaluation:")]
    assert len(hashes) == len(evaluations) == 2
    assert hashes[0] == hashes[1] and evaluations[0] == evaluations[1], (fen(board), hashes, evaluations)
    return hashes[0], evaluations[0]

def verify_variant(engine, pext):
    option = str(pext).lower()
    exchange(engine, f"setoption name Use PEXT value {option}\nsetoption name UCI_Chess960 value true")
    coverage = Counter()

    def exercise_castles(board):
        for move in board.generate_castling_moves():
            child = board.copy(stack=False)
            child.push(move)
            king_to = child.king(board.turn)
            rook_to = chess.square(5 if chess.square_file(king_to) == 6 else 3, chess.square_rank(king_to))
            kind = (move.from_square == king_to, move.to_square == rook_to,
                    move.from_square == rook_to, move.to_square == king_to)
            if not coverage[kind]:
                via_move = f"position fen {fen(board)} moves {move.uci()}"
                assert divide(engine, child, 2, via_move) == divide(engine, child, 2)
                # Search outputs king-to-rook UCI in both bestmove and PV.
                engine.send(f"position fen {fen(board)}\ngo depth 3 searchmoves {move.uci()}")
                search = engine.receive("bestmove")
                assert search[-1].split()[1] == move.uci(), search
                for line in search:
                    if " pv " in line:
                        pv_board = board.copy(stack=False)
                        for token in line.split(" pv ", 1)[1].split():
                            pv_board.push_uci(token)
            coverage[kind] += 1

    for index in range(960):
        board = chess.Board.from_chess960_pos(index)
        divide(engine, board, 2)
        for square, piece in list(board.piece_map().items()):
            if piece.piece_type not in (chess.KING, chess.ROOK):
                board.remove_piece_at(square)
        divide(engine, board, 2 if index % 12 == 0 else 1)
        exercise_castles(board)
    for back, rights in (("4KR2", "F"), ("5KR1", "G"), ("6KR", "H"), ("2RK4", "C")):
        board = chess.Board(f"4k3/8/8/8/8/8/8/{back} w {rights} - 0 1", chess960=True)
        for position in (board, board.mirror()):
            divide(engine, position, 3)
            exercise_castles(position)

    # The rook origin is part of the position identity even with the same pieces.
    inner = chess.Board("4k3/8/8/8/8/8/8/4KR1R w F - 0 1", chess960=True)
    outer = chess.Board("4k3/8/8/8/8/8/8/4KR1R w H - 0 1", chess960=True)
    assert divide(engine, inner, 2)[0] != divide(engine, outer, 2)[0]
    for text in (
        "4k3/8/8/8/8/8/8/rR2K3 w B - 0 1",
        "4k3/8/8/8/8/8/7b/R1K4R w HA - 0 1",
        "r1k4r/7B/8/8/8/8/8/4K3 b ha - 0 1",
    ):
        divide(engine, chess.Board(text, chess960=True), 3)
    for index in (0, 169, 518, 959):
        board = chess.Board.from_chess960_pos(index)
        divide(engine, board, 2, "position fen " + board.fen())
        rng = random.Random(index)
        for _ in range(14):
            moves = list(board.legal_moves)
            if not moves:
                break
            board.push(rng.choice(moves))
            divide(engine, board, 2)
    assert any(king and not rook for king, rook, _, _ in coverage)
    assert any(rook and not king for king, rook, _, _ in coverage)
    assert any(overlap and reverse for _, _, overlap, reverse in coverage)
    print(f"PEXT {option}: all 960 starts, 960 cleared back ranks and {sum(coverage.values())} legal castles", flush=True)

def orthodox_trace(engine, board):
    first = exchange(engine, f"ucinewgame\nposition fen {fen(board)}\nd\neval")
    engine.send("go depth 7")
    output = engine.receive("bestmove")
    signature = [line for line in first if line.startswith(("Evaluation:", "Zobrist Hash:"))]
    signature.extend(re.sub(r"\b(?:time|nps|hashfull) \d+ ?", "", line) for line in output
                     if line.startswith(("info depth ", "bestmove ")))
    return signature
